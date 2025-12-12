#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fstream>
#include <filesystem>

using namespace std;

static string trim(const string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static vector<string> split(const string &s) {
    vector<string> out;
    istringstream iss(s);
    string t;
    while (iss >> t) out.push_back(t);
    return out;
}

static string remove_quotes(const string &s) {
    if (s.size() >= 2 && 
       ((s.front()=='"' && s.back()=='"') ||
        (s.front()=='\'' && s.back()=='\'')))
        return s.substr(1, s.size()-2);
    return s;
}

static void builtin_echo(const string &line) {
    string trimmed = trim(line.substr(4)); 
    cout << remove_quotes(trimmed) << "\n";
}

static void builtin_env(const string &line) {
    string key = trim(line.substr(2)); 
    if (key.empty() || key[0] != '$') {
        cout << "\\e: command not found\n";
        return;
    }
    const char *val = getenv(key.c_str() + 1);
    if (!val) return;

    string v(val);
    if (v.find(':') == string::npos) {
        cout << v << "\n";
    } else {
        string part;
        stringstream ss(v);
        while (getline(ss, part, ':'))
            cout << part << "\n";
    }
}

static void builtin_debug(const string &line) {
    string msg = trim(line.substr(5));
    msg = remove_quotes(msg);
    cout << msg << "\n";
}

int main() {
    ios::sync_with_stdio(false);

    string line;
    while (true) {
        if (!getline(cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;

        if (line == "\\q") break;

        if (line.rfind("debug ",0)==0) {
            builtin_debug(line);
            continue;
        }
        if (line.rfind("echo ",0)==0) {
            builtin_echo(line);
            continue;
        }
        if (line.rfind("\\e ",0)==0) {
            builtin_env(line);
            continue;
        }

        // EXEC
        vector<string> args = split(line);
        vector<char*> cargs;
        for (auto &s : args) cargs.push_back(const_cast<char*>(s.c_str()));
        cargs.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execvp(cargs[0], cargs.data());
            perror("execvp");
            _exit(1);
        } else {
            int st;
            waitpid(pid, &st, 0);
        }
    }
    return 0;
}
