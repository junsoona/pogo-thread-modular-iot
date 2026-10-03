// The OTBR helper path for Power Base enrollment, without an OTBR: the
// request/answer protocol and its input checks, the bounded subprocess
// runner, and the real pogohome-otbr-helper (test build) driven by the real
// HelperCommissioner against tests/fake_ot_ctl.cpp -- checking that the PSKd
// only ever travels on stdin, never in any process's arguments or in an
// answer.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pogohome_api/commissioner.hpp"
#include "pogohome_api/otbr_helper_protocol.hpp"
#include "pogohome_api/subprocess.hpp"
#include "test_check.hpp"

using namespace pogohome_api;

namespace {

const char kHelper[] = POGOHOME_TEST_OTBR_HELPER;
const std::string kFakeDir = POGOHOME_TEST_FAKE_OTBR_DIR;
const char kEui[] = "744dbdfffe65d357";
const char kPskd[] = "J01NME7K2"; // a made-up test value

void WriteFake(const std::string &name, const std::string &content)
{
    std::ofstream out(kFakeDir + "/" + name, std::ios::trunc);
    out << content;
}

std::string ReadFake(const std::string &name)
{
    std::ifstream in(kFakeDir + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void ResetFake(const std::string &state, const std::string &mode = "")
{
    WriteFake("state", state);
    WriteFake("mode", mode);
    WriteFake("joiners", "");
    WriteFake("argv.log", "");
    WriteFake("stdin.log", "");
}

// Runs the helper directly with `request` on stdin (and optional args).
SubprocessResult RunHelper(const std::string &request, const std::vector<std::string> &extra_args = {})
{
    std::vector<std::string> argv = {kHelper};
    argv.insert(argv.end(), extra_args.begin(), extra_args.end());
    std::string input = request;
    SubprocessOptions options;
    options.timeout = std::chrono::milliseconds(8000);
    return RunSubprocess(argv, {}, input, options);
}

HelperCommissioner MakeCommissioner(std::chrono::milliseconds timeout = std::chrono::milliseconds(8000))
{
    HelperCommissionerConfig config;
    config.command = {kHelper};
    config.start_timeout = timeout;
    config.call_timeout = timeout;
    return HelperCommissioner(config);
}

} // namespace

void RunOtbrHelperTests(int *failures)
{
    printf("\n-- scenario: EUI-64 and PSKd checks (shared by the bridge and the helper) --\n");
    {
        API_CHECK(IsValidEui64("744dbdfffe65d357"), "16 lowercase hex digits are an EUI-64");
        API_CHECK(!IsValidEui64("744DBDFFFE65D357"), "the helper side accepts lowercase only");
        API_CHECK(!IsValidEui64("*"), "the wildcard Joiner is not an EUI-64");
        API_CHECK(!IsValidEui64("744dbdfffe65d35"), "15 digits are refused");
        API_CHECK(!IsValidEui64("744dbdfffe65d3570"), "17 digits are refused");
        API_CHECK(!IsValidEui64("744dbdfffe65d35g"), "a non-hex digit is refused");
        API_CHECK(!IsValidEui64("0000000000000000"), "all zero is refused");
        API_CHECK(!IsValidEui64("74:4d:bd:ff:fe:65"), "separators are refused");

        API_CHECK(IsValidPskd("J01NME"), "6 characters of the Thread PSKd alphabet are valid");
        API_CHECK(IsValidPskd(std::string(32, 'A')), "32 characters are valid");
        API_CHECK(!IsValidPskd("J01NM"), "5 characters are refused");
        API_CHECK(!IsValidPskd(std::string(33, 'A')), "33 characters are refused");
        API_CHECK(!IsValidPskd("j01nme"), "lowercase is refused");
        API_CHECK(!IsValidPskd("ABCDEI") && !IsValidPskd("ABCDEO") && !IsValidPskd("ABCDEQ") &&
                      !IsValidPskd("ABCDEZ"),
                  "I, O, Q and Z are refused");
        API_CHECK(!IsValidPskd("ABC DEF") && !IsValidPskd("ABC-DEF") && !IsValidPskd("ABCDE*"),
                  "spaces and punctuation are refused");
    }

    printf("\n-- scenario: the one-line request protocol --\n");
    {
        HelperRequest add;
        add.op = HelperOp::JoinerAdd;
        add.eui64 = kEui;
        add.timeout_s = 120;
        add.pskd = kPskd;
        const std::string line = BuildHelperRequest(add);
        API_CHECK(line == std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 " + kPskd + "\n",
                  "a joiner-add request is one fixed-format line");
        HelperRequest parsed;
        API_CHECK(ParseHelperRequest(line, parsed) && parsed.op == HelperOp::JoinerAdd && parsed.eui64 == kEui &&
                      parsed.timeout_s == 120 && parsed.pskd == kPskd,
                  "it parses back to the same request");

        add.eui64 = "*";
        API_CHECK(BuildHelperRequest(add).empty(), "no request can be built for the wildcard Joiner");
        add.eui64 = kEui;
        add.timeout_s = 9;
        API_CHECK(BuildHelperRequest(add).empty(), "a Joiner timeout under 10 s is refused");
        add.timeout_s = 301;
        API_CHECK(BuildHelperRequest(add).empty(), "a Joiner timeout over 300 s is refused");

        auto rejects = [&](const std::string &raw) {
            HelperRequest r;
            return !ParseHelperRequest(raw, r);
        };
        API_CHECK(rejects(std::string("pogohome-otbr/1 joiner-add * 120 ") + kPskd + "\n"),
                  "the helper refuses a wildcard Joiner on its own");
        API_CHECK(rejects(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 j01nme\n"),
                  "the helper refuses a lowercase PSKd on its own");
        API_CHECK(rejects(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 0120 " + kPskd + "\n"),
                  "a timeout with a leading zero is refused");
        API_CHECK(rejects(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 " + kPskd),
                  "a request without its newline is refused");
        API_CHECK(rejects(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 " + kPskd + "\nx"),
                  "anything after the newline is refused");
        API_CHECK(rejects("pogohome-otbr/1 commissioner-state\npogohome-otbr/1 commissioner-state\n"),
                  "a second request line is refused");
        API_CHECK(rejects("pogohome-otbr/1  commissioner-state\n"), "a double space is refused");
        API_CHECK(rejects("pogohome-otbr/1 commissioner-stop\n"), "commissioner-stop does not exist");
        API_CHECK(rejects("pogohome-otbr/1 dataset active\n") && rejects("pogohome-otbr/1 networkkey\n"),
                  "no Dataset or Network Key operation exists");
        API_CHECK(rejects("pogohome-otbr/1 commissioner-state x\n"), "extra words are refused");
        API_CHECK(rejects("pogohome-otbr/2 commissioner-state\n"), "another protocol version is refused");
        API_CHECK(rejects(std::string(kMaxHelperRequestBytes, 'A') + "\n"), "an oversized request is refused");
    }

    printf("\n-- scenario: ot-ctl output and helper answers --\n");
    {
        OtCliOutput out = ParseOtCliOutput("active\r\nDone\r\n");
        API_CHECK(out.kind == OtCliOutput::Kind::Done && out.lines.size() == 1 && out.lines[0] == "active",
                  "a state and Done");
        out = ParseOtCliOutput("> commissioner joiner remove x\r\nError 23: NotFound\r\n");
        API_CHECK(out.kind == OtCliOutput::Kind::Error && out.error_name == "NotFound",
                  "an echoed command then an error names the error");
        API_CHECK(!OtCliOutputComplete("active\r\nDon"), "an unfinished line decides nothing");

        bool ok = false;
        std::vector<std::string> words;
        API_CHECK(ParseHelperResponse("ok active started\n", ok, words) && ok && words.size() == 2,
                  "an ok answer parses");
        API_CHECK(ParseHelperResponse("err otbr_unavailable\n", ok, words) && !ok && words[0] == "otbr_unavailable",
                  "an error answer parses");
        API_CHECK(!ParseHelperResponse("ok J01NME\n", ok, words), "an answer with anything but [a-z_] is refused");
        API_CHECK(!ParseHelperResponse("", ok, words) && !ParseHelperResponse("maybe\n", ok, words),
                  "an empty or unknown answer is refused");
    }

    printf("\n-- scenario: the subprocess runner is bounded --\n");
    {
        std::string none;
        SubprocessOptions options;
        options.timeout = std::chrono::milliseconds(300);
        auto t0 = std::chrono::steady_clock::now();
        SubprocessResult r = RunSubprocess({"/bin/sleep", "5"}, {}, none, options);
        auto took = std::chrono::steady_clock::now() - t0;
        API_CHECK(r.spawned && r.timed_out && !r.exited, "a child that runs too long is killed");
        API_CHECK(took < std::chrono::seconds(2), "and the call returns soon after its timeout");

        options.timeout = std::chrono::milliseconds(3000);
        options.max_stdout_bytes = 1024;
        r = RunSubprocess({"/usr/bin/yes"}, {}, none, options);
        API_CHECK(r.output_overflow && r.out.size() <= 1024, "a child that prints too much is killed");

        r = RunSubprocess({"bin/sh"}, {}, none, options);
        API_CHECK(!r.spawned, "a relative program path is never run");
        r = RunSubprocess({"/nonexistent/program"}, {}, none, options);
        API_CHECK(r.exited && r.exit_code == 127, "a missing program exits 127");

        std::string secret = "J01NME7K2";
        r = RunSubprocess({"/bin/cat"}, {}, secret, options);
        API_CHECK(r.exited && r.exit_code == 0 && r.out == "J01NME7K2", "stdin data reaches the child");
        API_CHECK(secret.empty(), "the caller's copy of the stdin data is wiped");
    }

    printf("\n-- scenario: the helper refuses bad input on its own --\n");
    {
        ResetFake("active");
        SubprocessResult r = RunHelper("pogohome-otbr/1 commissioner-state\n", {"commissioner", "state"});
        API_CHECK(r.exited && r.exit_code == 2 && r.out == "err invalid_request\n",
                  "any command-line argument is refused");
        r = RunHelper(std::string("pogohome-otbr/1 joiner-add * 120 ") + kPskd + "\n");
        API_CHECK(r.out == "err invalid_request\n", "a wildcard Joiner is refused");
        r = RunHelper(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 SHORT\n");
        API_CHECK(r.out == "err invalid_request\n", "a 5-character PSKd is refused");
        r = RunHelper(std::string(200, 'A'));
        API_CHECK(r.out == "err invalid_request\n", "an oversized request is refused");
        r = RunHelper("pogohome-otbr/1 commissioner-state\n; reboot\n");
        API_CHECK(r.out == "err invalid_request\n", "a second line is refused");
        API_CHECK(ReadFake("argv.log").empty(), "ot-ctl never ran for any refused request");
    }

    printf("\n-- scenario: HelperCommissioner -> helper -> ot-ctl (fake) --\n");
    {
        ResetFake("disabled");
        HelperCommissioner commissioner = MakeCommissioner();
        std::string state;
        API_CHECK(commissioner.QueryState(state) == CommissionerStatus::Ok && state == "disabled",
                  "the Commissioner state is read");
        bool started = false;
        API_CHECK(commissioner.EnsureActive(started) == CommissionerStatus::Ok && started,
                  "a disabled Commissioner is started and waited for until active");
        API_CHECK(commissioner.EnsureActive(started) == CommissionerStatus::Ok && !started,
                  "an already active Commissioner is left as it is");

        std::string pskd = kPskd;
        API_CHECK(commissioner.AddJoiner(kEui, pskd, 120) == CommissionerStatus::Ok, "the exact EUI-64 is allowed");
        API_CHECK(pskd.empty(), "the caller's PSKd is wiped");
        API_CHECK(ReadFake("joiners").find(kEui) != std::string::npos, "the OTBR has that one Joiner entry");
        const std::string argv_log = ReadFake("argv.log");
        API_CHECK(!argv_log.empty() && argv_log.find(kPskd) == std::string::npos &&
                      argv_log.find("args=\n") != std::string::npos,
                  "ot-ctl got no command arguments at all, so never the PSKd");
        API_CHECK(ReadFake("stdin.log").find(std::string("commissioner joiner add ") + kEui + " " + kPskd + " 120") !=
                      std::string::npos,
                  "the CLI command, PSKd included, went to ot-ctl's stdin");

        API_CHECK(commissioner.RemoveJoiner(kEui) == CommissionerStatus::Ok, "the entry is removed");
        API_CHECK(ReadFake("joiners").find(kEui) == std::string::npos, "the OTBR no longer has it");
        API_CHECK(commissioner.RemoveJoiner(kEui) == CommissionerStatus::NotFound,
                  "removing an entry that is already gone is NotFound (a successful cleanup)");
    }

    printf("\n-- scenario: echoing ot-ctl never leaks the PSKd into an answer --\n");
    {
        ResetFake("active", "echo");
        SubprocessResult r = RunHelper(std::string("pogohome-otbr/1 joiner-add ") + kEui + " 120 " + kPskd + "\n");
        API_CHECK(r.out == "ok added\n", "the answer is the fixed word, not ot-ctl's output");
        API_CHECK(r.out.find(kPskd) == std::string::npos && r.err.find(kPskd) == std::string::npos,
                  "neither stdout nor stderr of the helper contains the PSKd");
    }

    printf("\n-- scenario: helper and OTBR failures --\n");
    {
        ResetFake("disabled", "unavailable");
        HelperCommissioner commissioner = MakeCommissioner();
        std::string state;
        API_CHECK(commissioner.QueryState(state) == CommissionerStatus::Unavailable,
                  "ot-ctl that cannot open the OTBR socket is Unavailable");

        ResetFake("disabled");
        std::string pskd = kPskd;
        API_CHECK(commissioner.AddJoiner(kEui, pskd, 120) == CommissionerStatus::InvalidState,
                  "adding a Joiner while the Commissioner is not active is InvalidState");

        ResetFake("active", "hang");
        auto t0 = std::chrono::steady_clock::now();
        API_CHECK(commissioner.QueryState(state) == CommissionerStatus::Timeout,
                  "ot-ctl that never answers makes the helper answer err timeout");
        API_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4),
                  "within the helper's own ot-ctl timeout");

        HelperCommissioner impatient = MakeCommissioner(std::chrono::milliseconds(500));
        t0 = std::chrono::steady_clock::now();
        API_CHECK(impatient.QueryState(state) == CommissionerStatus::Timeout,
                  "a helper that does not answer in time is a Timeout for the bridge");
        API_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "the bridge does not wait longer");

        HelperCommissionerConfig missing;
        missing.command = {"/nonexistent/pogohome-otbr-helper"};
        HelperCommissioner absent(missing);
        API_CHECK(absent.QueryState(state) != CommissionerStatus::Ok, "a missing helper is never Ok");

        HelperCommissionerConfig garbage;
        garbage.command = {"/bin/echo", "ok", "J01NME"};
        HelperCommissioner odd(garbage);
        API_CHECK(odd.QueryState(state) == CommissionerStatus::Failed, "an unexpected answer is Failed");
        ResetFake("disabled");
    }
}
