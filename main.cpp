#include <iostream>
#include <fstream>
#include <regex>
#include <iomanip>
#include <sstream>
#include <tuple>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef WIN32
#include <conio.h>
#else
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "my_sleep.h"
#include "conf.h"
#include "IperfProc.h"
#include "MyNetwork.h"
#include "DbHandling.h"
#include "Progress.h"


std::map<std::string, std::string> conf;

int mykbhit(void)
{
  struct termios oldt, newt;
  int ch;
  int oldf;

  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

  ch = getchar();

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  if(ch != EOF)
  {
    ungetc(ch, stdin);
    return 1;
  }

  return 0;
}

// Get current date/time, format is YYYY-MM-DD.HH:mm:ss
const std::string currentDateTime() {
	time_t     now = time(0);
	struct tm  tstruct;
	char       buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);

	return buf;
}

// Calls process in background and checks it's output.
int StreamCommand(const std::string& command, LineProcessor& proc,
                  const std::string& tempFileName, bool redirect = true) {
    using namespace std;
    const int MAX_RETRIES_OPEN = 50;
    const int REFRESH_MS = 100;
    const int MAX_RETRIES_READ = 5000;

	std::remove(tempFileName.c_str());
    string cmd_line = "./" + command;
    if(redirect) {
        cmd_line += " > " + tempFileName;
    };
    cmd_line += " &";
    if(system(cmd_line.c_str()) != 0) {
        cout << endl << "ERROR: Failed to call command" << endl;
        return -1;
    }
    // Wait until file is created by system call.
    ifstream stream_results(tempFileName);
    int retries = MAX_RETRIES_OPEN;
    while(retries-- && !stream_results.is_open()) {
        delay(REFRESH_MS);
        stream_results.open(tempFileName);
    }
    if(!stream_results.is_open()) {
        cout << endl << "ERROR: Failed to execute command & get output" << endl;
        return -1;
    }

    retries = MAX_RETRIES_READ;
    while(retries && stream_results.is_open()) {
        string line;
        if(std::getline(stream_results, line)) {
            retries = MAX_RETRIES_READ;
            if(!proc.Process(line)) {
                break;
            }
        } else {
            if(stream_results.eof()) {
                stream_results.clear();
            }
            delay(REFRESH_MS);
            stream_results.sync();
            retries--;
        }
    }
    stream_results.close();
    if(retries == 0) {
        cout << endl << "ERROR: Command did not return expected results" << endl;
		std::remove(tempFileName.c_str());
        return -1;
    }
	std::remove(tempFileName.c_str());
    return 0;
}

int HandlePing(int& latency) {
    using namespace std;
    string ping_cmd_line = conf["PING_CMD"] +
        " | grep \"round\\-trip\" > ping_results.txt";
    if(system(ping_cmd_line.c_str()) != 0) {
        cout << endl <<  "ERROR: Failed to call ping" << endl;
        return -1;
    }
    ifstream ping_results("ping_results.txt");
    if(!ping_results.is_open()) {
        cout << endl << "ERROR: Failed to retrieve ping results from file" << endl;
		std::remove("ping_results.txt");
        return -1;
    }
    string results;
    getline(ping_results, results);
    smatch match;
    regex re("stddev = .*/(.*)/");
    if(!regex_search(results, match, re) || match.size() < 2) {
        cout << endl << "ERROR: Failed to parse ping result" << endl;
		std::remove("ping_results.txt");
        return -1;
    }
    latency = (int)atof(match.str(1).c_str());
    ping_results.close();
	std::remove("ping_results.txt");
    return 0;
}

int HandleIperf(const Progress& p, int& downloadSpeed, int& uploadSpeed, std::string& downloadTime, std::string& uploadTime,
	std::string& downloadDuration, std::string& uploadDuration) {
    using namespace std;
    IperfProc procDown(p, IperfProc::DOWNLOAD);
    IperfProc procUp(p, IperfProc::UPLOAD);

	p.ShowProgress(Progress::DOWN_CON, 0);
	downloadTime = currentDateTime();
    Timer tmr;
    StreamCommand(conf["IPERF_DOWN_CMD"], procDown, "download_stream.txt", false);
    if(procDown.getSuccess()) {
        downloadSpeed = (int)procDown.getSum();
    }
    else {
        cout << endl << "ERROR: Failed to get average download speed from iPerf3" << endl;
        return -1;
    }
	stringstream ss;
	ss << fixed << setprecision(2) << tmr.elapsed();
	downloadDuration = ss.str();
	ss.str(std::string());
	ss.clear();

	p.ShowProgress(Progress::UP_CON, 0);
	uploadTime = currentDateTime();
	tmr.reset();
    StreamCommand(conf["IPERF_UP_CMD"], procUp, "upload_stream.txt", false);
    if(procUp.getSuccess()) {
        uploadSpeed = (int)procUp.getSum();
    }
    else {
        cout << endl << "ERROR: Failed to get average upload speed from iPerf3" << endl;
        return -1;
    }
	ss << fixed << setprecision(2) << tmr.elapsed();
	uploadDuration = ss.str();
	ss.str(std::string());
	ss.clear();
	p.ShowProgress(Progress::UP_END, 1);
    return 0;
}

int HandleDb(const std::map<std::string, std::string>& params) {
    using namespace std;
	DbHandling dbObj(conf);
	dbObj.Store(params);
    return 0;
}

int HandleExtIp(MyNetwork& n, std::string& extIp) {
    extIp = n.GetExtIp();
    return extIp.length() ? 0 : -1;
}

int HandleResults(const Progress& p, MyNetwork& n, const std::map<std::string, std::string>& params) {
	using namespace std;
    string testId = n.GetTestId(params);
	if (testId.size() == 0) {
		return -1;
	}
	p.ShowProgress(Progress::RESULTS, 1);
	return n.DisplayResults(testId);
}

int main() {
    using namespace std;

	cout << string(70, '=') << endl;
	cout << "Singtel Speed Testing for 10G Network" << endl << endl;
	cout << "Powered by iPerf3" << endl;
	cout << string(70, '=') << endl;
	cin.clear();
	cout << "Press Any Key to Continue....." << endl;
#ifdef WIN32
    fflush(stdin);
	_getch();
#else
    while(!mykbhit()) { delay(10); }
#endif
	cout << "Testing in Progress" << endl;

	Progress::ShowProgress(0);

    if(conf_read("config.txt", conf) != 0) {
        cout << endl << "ERROR: Failed to read configuration file" << endl;
        return -1;
    }
	Progress p(conf);

    MyNetwork net(conf);

    map<string, string> params;
    int ret = 0;
    int latency = 0;
    if((ret = HandlePing(latency)) != 0) {
        return ret;
    }
	p.ShowProgress(Progress::PING, 1);

    int downloadSpeed = 0, uploadSpeed = 0;
	string downloadTime, uploadTime, downloadDuration, uploadDuration;
    if((ret = HandleIperf(p, downloadSpeed, uploadSpeed, downloadTime, uploadTime, downloadDuration, uploadDuration)) != 0) {
        return ret;
    }

    string extIp;
    if((ret = HandleExtIp(net, extIp)) != 0) {
        return ret;
    }

    params["EXT_IP"] = extIp;
	params["LATENCY"] = to_string(latency);
	params["DOWNLOAD_TIME"] = downloadTime;
	params["DOWNLOAD_DURATION"] = downloadDuration;
	params["DOWNLOAD_SPEED"] = to_string(downloadSpeed);
	params["UPLOAD_TIME"] = uploadTime;
	params["UPLOAD_DURATION"] = uploadDuration;
	params["UPLOAD_SPEED"] = to_string(uploadSpeed);

	/*cout << "EXT_IP: '" << params["EXT_IP"] << "'" << endl;
	cout << "LATENCY: " << params["LATENCY"] << endl;
	cout << "DOWNLOAD_TIME: " << params["DOWNLOAD_TIME"] << endl;
	cout << "DOWNLOAD_DURATION: " << params["DOWNLOAD_DURATION"] << endl;
	cout << "DOWNLOAD_SPEED: " << params["DOWNLOAD_SPEED"] << endl;
	cout << "UPLOAD_TIME: " << params["UPLOAD_TIME"] << endl;
	cout << "UPLOAD_DURATION: " << params["UPLOAD_DURATION"] << endl;
	cout << "UPLOAD_SPEED: " << params["UPLOAD_SPEED"] << endl;*/

	p.ShowProgress(Progress::DB, 0);
    if((ret = HandleDb(params)) != 0) {
		// Ignore DB errors - only for logging purposes.
        //return ret;libiperf
    }
	p.ShowProgress(Progress::RESULTS, 0);
    if((ret = HandleResults(p, net, params)) != 0) {
        return ret;
    }
	Progress::ShowProgress(100);
	cout << endl << "Completed" << endl;
#ifdef WIN32
    fflush(stdin);
	_getch();
#endif
    return 0;
}
