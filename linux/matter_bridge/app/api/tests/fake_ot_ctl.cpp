// A stand-in for OpenThread's ot-ctl, for the host tests of
// pogohome-otbr-helper only (the test build of the helper runs this instead
// of /usr/sbin/ot-ctl). Like ot-ctl with no command arguments it reads CLI
// commands from stdin and answers each with output lines ending in "Done" or
// "Error <n>: <Name>" (CRLF), and exits at stdin EOF.
//
// Its state lives next to the binary (tests reset it between scenarios):
//   state    Commissioner state: disabled | petitioning | active
//            (petitioning becomes active after one "commissioner state")
//   joiners  one allowed EUI-64 per line
//   mode     "" | unavailable (no session, like the real permission error)
//            | hang (never answers) | echo (echoes each command, like a
//            readline build does)
//   mleid    what "ipaddr mleid" prints before "Done", verbatim (may be
//            several lines, or empty); "ERROR" = answer Error 13 instead
//   argv.log each run's arguments after argv[0], one line per run
//   stdin.log every command line received (a test fake: the PSKd written
//            here is a made-up test value)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string g_dir;

std::string ReadFile(const std::string &name)
{
    std::ifstream in(g_dir + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

void WriteFile(const std::string &name, const std::string &content)
{
    std::ofstream out(g_dir + "/" + name, std::ios::trunc);
    out << content;
}

void AppendFile(const std::string &name, const std::string &line)
{
    std::ofstream out(g_dir + "/" + name, std::ios::app);
    out << line << "\n";
}

void Print(const std::string &s)
{
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
}

std::vector<std::string> Words(const std::string &line)
{
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w) {
        out.push_back(w);
    }
    return out;
}

void Handle(const std::string &line)
{
    std::vector<std::string> w = Words(line);
    if (w.size() == 2 && w[0] == "commissioner" && w[1] == "state") {
        std::string state = ReadFile("state");
        if (state.empty()) {
            state = "disabled";
        }
        Print(state + "\r\nDone\r\n");
        if (state == "petitioning") {
            WriteFile("state", "active");
        }
        return;
    }
    if (w.size() == 2 && w[0] == "ipaddr" && w[1] == "mleid") {
        std::string mleid = ReadFile("mleid");
        if (mleid == "ERROR") {
            Print("Error 13: InvalidState\r\n");
            return;
        }
        std::string out;
        for (char c : mleid) {
            out += c == '\n' ? std::string("\r\n") : std::string(1, c);
        }
        Print(out + (out.empty() ? "" : "\r\n") + "Done\r\n");
        return;
    }
    if (w.size() == 2 && w[0] == "commissioner" && w[1] == "start") {
        if (ReadFile("state") == "active") {
            Print("Error 24: Already\r\n");
            return;
        }
        WriteFile("state", "petitioning");
        Print("Commissioner: petitioning\r\nDone\r\n");
        return;
    }
    if (w.size() >= 3 && w[0] == "commissioner" && w[1] == "joiner") {
        if (ReadFile("state") != "active") {
            Print("Error 13: InvalidState\r\n");
            return;
        }
        std::string joiners = ReadFile("joiners");
        if (w[2] == "add" && (w.size() == 5 || w.size() == 6)) {
            if (joiners.find(w[3]) == std::string::npos) {
                AppendFile("joiners", w[3]);
            }
            Print("Done\r\n");
            return;
        }
        if (w[2] == "remove" && w.size() == 4) {
            size_t at = joiners.find(w[3]);
            if (at == std::string::npos) {
                Print("Error 23: NotFound\r\n");
                return;
            }
            joiners.erase(at, w[3].size());
            WriteFile("joiners", joiners);
            Print("Done\r\n");
            return;
        }
    }
    Print("Error 35: InvalidCommand\r\n");
}

} // namespace

int main(int argc, char **argv)
{
    std::string self = argv[0];
    size_t slash = self.rfind('/');
    g_dir = slash == std::string::npos ? "." : self.substr(0, slash);

    std::string args;
    for (int i = 1; i < argc; i++) {
        args += (i > 1 ? " " : "") + std::string(argv[i]);
    }
    AppendFile("argv.log", "argc=" + std::to_string(argc) + " args=" + args);

    const std::string mode = ReadFile("mode");
    if (mode == "unavailable") {
        std::fprintf(stderr, "connect session failed: Permission denied\n");
        return 1;
    }

    char buf[512];
    while (std::fgets(buf, sizeof(buf), stdin) != nullptr) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        AppendFile("stdin.log", line);
        if (mode == "hang") {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return 0;
        }
        if (mode == "echo") {
            Print("> " + line + "\r\n");
        }
        Handle(line);
    }
    return 0;
}
