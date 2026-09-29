# Submission Report

- Submission generated at 09/29/2026 at 23:35:18

- Machine info: Linux runnervm8df0l 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

## Note to Students

Please read this report carefully before submission.
Ensure that all sections are complete and accurate.
Look for any errors in the build or test outputs.
If you find any issues, correct them before submitting.
Post any questions on the class discussion board for help.


---

## README

# CS425 Project 2

This repository implements reliable file transfer over UDP using the packet format and Go-Back-N protocol. First, to get the program compiled, tested and setup:

```sh
make clean
make test
make check
make all
```

To try a transfer, use three terminals. Keep the relay running in Terminal 1, start the receiver before the sender, and use the same relay port in all three commands. The following example also creates the input file that the sender will read.

**Terminal 1 — relay**

```bash
python3 cs425_relay.py --port 4250 --delay 50 --seed 7
```

**Terminal 2 — receiver**

```bash
./build/release/myapp recv -s my-session -p 4250 127.0.0.1 output.bin
```

**Terminal 3 — sender and comparison**

```bash
printf 'Hello from CS425.\n' > input.bin
./build/release/myapp send -s my-session -p 4250 -w 16 127.0.0.1 input.bin
cmp input.bin output.bin && echo "transfer verified"
```

Notes:
 - The receiver must start before the sender.
 - The sender and receiver must use the same session name.
 - Use a separate terminal for the relay, receiver, and sender.
 - Start a fresh receiver and use a new output file for every transfer.

After the clean transfer works, start a fresh receiver in Terminal 2 (the previous receiver exits after its two-second FIN linger), then exercise the unreliable channel from Terminal 3 with:

```bash
./build/release/myapp recv -s lossy-session -p 4250 127.0.0.1 output.bin
```

Use the matching session in Terminal 3:

```bash
./build/release/myapp send -s lossy-session -p 4250 -w 16 \
  -l 0.1 -c 0.05 -d 0.05 127.0.0.1 input.bin
cmp input.bin output.bin && echo "lossy transfer verified"
```

## Validation

The Unity suite covers the checksum, packet validation, sender and receiver state transitions, retransmission, FIN handling, empty files, exact 1024-byte multiples, and a fixed-seed in-memory transfer with independent 20% loss, corruption, and duplication in both directions. Full validation can be ran with:

```sh
make clean
make all
make check
make report
make leak
make leak-test
```

`make report` requires `gcovr` and reports both line and branch coverage. The protocol tests reach 100% of executable lines and measured branches in `src/lab.c`, without excluding protocol logic. Integration tests use a fixed relay seed and verify the received file with `cmp`. 

Only untestable items (sys calls, their resulting failures, etc) are excluded from testing.

## Task 6: measuring the window

The following is a manual procedure for the four required configurations. It
uses only three terminals and the shell's built-in `time` command; no helper
script is required.

First create the 1 MiB input file once:

```sh
head -c 1048576 /dev/urandom > 1mib.bin
```

Start the relay in **Terminal 1** and leave it running for all twelve runs:

```sh
python3 cs425_relay.py --port 4250 --delay 50 --seed 7
```

For each run, start a new receiver in **Terminal 2**, replacing both
`RUN_SESSION` and `RUN_OUTPUT` with values that have not been used before:

```sh
./build/release/myapp recv -s RUN_SESSION -p 4250 127.0.0.1 RUN_OUTPUT
```

Then run the sender in **Terminal 3**. Replace `WINDOW`, `LOSS`, and
`RUN_SESSION` with the values for that row:

```sh
time -p ./build/release/myapp send \
  -s RUN_SESSION -p 4250 -w WINDOW -T 250 \
  -l LOSS -c 0 -d 0 127.0.0.1 1mib.bin
```

After the sender exits, verify the copy before recording the elapsed time:

```sh
cmp 1mib.bin RUN_OUTPUT && echo "transfer verified"
```

The receiver exits after its FIN linger. Do not reuse its output file or
session name.

## Known Bugs or Issues

None known.

## Experience

I'm not sure how much I learned here. I am genuinely trying to learn to do AI-first work on projects, but this one was a little rough for my learning. A lot of times, I had to go back and review something that seemed to make sense, but I later realized I hadn't really grasped. That being said, I think I get the general gist of the assignment fairly well.

It's also incredibly how little overhead there is. Less than 2% of the time spent actually handling things is impressive, and I imagine on dedicated hardware like a router or NIC it can be even faster. I don't know if running this on the same VM actually ever touches the NIC, but regardless it's neat. 

## Design

The project implements a 3 layer split (packet, Go-Back-N state machine, and I/O layer), though it is still within one file. This good practice keeps separate logic from getting muddled and enchances maintainability. It also renders some aspects of testing possible, as previously it would have required an OS level or exterior mocking to test.

`lab.c` contains the packet laer, including the `lab_checksum, lab_encode, lab_decode` functions. These deal in buffers and structs only, no sockets/files/clocks.

`lab.c` is also home to the Go-Back-N state machine layer, in the form of `lab_sender and lab_reciever`. They recieve events and timestamps, returning packet pointers/counts, ACKS, payloads and timer state updates.

`main.c` holds the I/O, and is the only place with UDP cognizance. It takes external events, passes them to the state machine, and then acts on the returned item (via either packet send or file write).

This keeps it modular, flexible and easy to test. In theory, any one design element could be swapped out relatively easily.

## Results

I ran each of these setups 3 times each, with a 50ms delay resulting in a 100ms RTT:

| Window | Loss | Corrupt | Dup |
|---:|---:|---:|---:|
| 1 | 0 | 0 | 0 |
| 16 | 0 | 0 | 0 |
| 1 | 0.05 | 0 | 0 |
| 16 | 0.05 | 0 | 0 |

And recorded the values here:

| Window | Loss | Mean time (s) | Throughput (KiB/s) |
|---:|---:|---:|---:|
| 1 | 0 | 104.3 | 9.81 |
| 16 | 0 | 6.64 | 154.22 |
| 1 | 0.05 | 131.38 | 7.79 |
| 16 | 0.05 | 20.36 | 50.29 |

The first row took ~2ms longer than it should have based purely on round trip time. This is reasonable to me. It also has to queue packets, wait for time from the scheduler, execute code, etc. There's a lot more going on.

A speedup from an increased window is expected, we're "parallelizing" the processing. What's...odd is that it's MORE than 16 times faster, closer to 16.5 times. My best guess is that this is along the lines of scheduler optimiziation. It would make sense that if it's a more consistent workload, it would stay executing for longer, preventing the time losses of waiting for a time slice again. I'm just speculating though.

As for the 0.05 loss setting, the results were mixed. It produced a greater absolute slowdown on the window 1 run (of about 25ms for w1 vs 15ms for w16), but much higher relative impact for window 16. I believe this is because when a packet is dropped, everything past it has to be retransmitted. You are (briefly) deleting the benefits of the larger window size.

---


## Build Output

This section was generated by running `make all` in the project root directory.

```bash
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/lab.c -o build/debug/lab.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug/main.c.o
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address build/debug/lab.c.o build/debug/main.c.o -o build/debug/myapp_d -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/lab.c -o build/release/lab.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/main.c -o build/release/main.c.o
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion build/release/lab.c.o build/release/main.c.o -o build/release/myapp 
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/lab.c -o build/tests/lab.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/main.c -o build/tests/main.c.o
mkdir -p build/tests/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/lab-test.c -o build/tests/lab-test.c.o
mkdir -p build/tests/harness/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/harness/unity.c -o build/tests/harness/unity.c.o
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage build/tests/lab.c.o build/tests/main.c.o build/tests/lab-test.c.o build/tests/harness/unity.c.o -o build/tests/myapp_t -fprofile-arcs -ftest-coverage
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/lab.c -o build/debug-test/lab.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug-test/main.c.o
mkdir -p build/debug-test/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/lab-test.c -o build/debug-test/lab-test.c.o
mkdir -p build/debug-test/harness/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/harness/unity.c -o build/debug-test/harness/unity.c.o
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address build/debug-test/lab.c.o build/debug-test/main.c.o build/debug-test/lab-test.c.o build/debug-test/harness/unity.c.o -o build/debug-test/myapp_td -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
Builds completed. You can run the application with: ./build/release/myapp
You can run the debug build with: ./build/debug/myapp_d
You can run the test build with: ./build/tests/myapp_t
You can run the debug-test build with: ./build/debug-test/myapp_td
```

---

## Coverage Report

This section was generated by running `make report` in the project root directory.

```bash
make BUILD=test
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: 'build/tests/myapp_t' is up to date.
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
./build/tests/myapp_t
tests/lab-test.c:1043:test_checksum_examples_and_odd_length:PASS
tests/lab-test.c:1044:test_packet_round_trip_and_validation:PASS
tests/lab-test.c:1045:test_packet_encode_rejects_bad_arguments:PASS
tests/lab-test.c:1046:test_packet_decode_rejects_each_header_error:PASS
tests/lab-test.c:1048:test_receiver_orders_and_reacknowledges:PASS
tests/lab-test.c:1049:test_receiver_fin_and_repeated_fin:PASS
tests/lab-test.c:1051:test_sender_window_ack_timeout_and_giveup:PASS
tests/lab-test.c:1052:test_empty_transfer_and_lossy_state_machine_transfer:PASS
tests/lab-test.c:1053:test_seeded_bidirectional_lossy_transfer:PASS
tests/lab-test.c:1054:test_empty_and_exact_multiple_transfers:PASS
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
myapp: invalid option -- 'x'
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
ERR no receiver
tests/lab-test.c:1055:test_main_cli_and_clean_transfer:PASS
tests/lab-test.c:1056:test_receiver_fwrite_failure:PASS

-----------------------
12 Tests 0 Failures 0 Ignored 
OK
mkdir -p ./build/report/html
mkdir -p ./build/report/txt
python -m gcovr -r . --html --html-details --exclude-directories build/tests/harness --exclude '.*test\.c$' -o ./build/report/html/coverage_report.html
(INFO) Reading coverage data...

(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:123:37 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:123:37 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:149:31 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:149:31 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:199:27 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:199:27 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:200:26 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:200:26 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:203:27 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:203:27 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:204:26 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:204:26 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:250:37 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:250:37 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:264:23 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:264:23 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:284:96 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:284:96 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:331:84 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:331:84 needs at least gcc-14 with supported JSON format.
(INFO) Writing coverage report...

python -m gcovr -r . --txt --txt-metric branch --exclude-directories build/tests/harness --exclude '.*test\.c$'
(INFO) Reading coverage data...

(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:123:37 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:123:37 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:149:31 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:149:31 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:199:27 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:199:27 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:200:26 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:200:26 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:203:27 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:203:27 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:204:26 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:204:26 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:250:37 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:250:37 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:264:23 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:264:23 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:284:96 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:284:96 needs at least gcc-14 with supported JSON format.
(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:331:84 needs at least gcc-14 with supported JSON format.
::warning::(WARNING) Source branch exclusion at /home/runner/work/cs425-p2/cs425-p2/src/main.c:331:84 needs at least gcc-14 with supported JSON format.
(INFO) Writing coverage report...

------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                    Branches    Taken  Cover   Missing
------------------------------------------------------------------------------
src/lab.c                                     94       94   100%
src/main.c                                   180      173    96%   123,149,202,250,263,284,331
------------------------------------------------------------------------------
TOTAL                                        274      267    97%
------------------------------------------------------------------------------
```

---

## Address Sanitizer Report

This section was generated by running `make leak-test` in the project root directory.

```bash
tests/lab-test.c:1043:test_checksum_examples_and_odd_length:PASS
tests/lab-test.c:1044:test_packet_round_trip_and_validation:PASS
tests/lab-test.c:1045:test_packet_encode_rejects_bad_arguments:PASS
tests/lab-test.c:1046:test_packet_decode_rejects_each_header_error:PASS
tests/lab-test.c:1048:test_receiver_orders_and_reacknowledges:PASS
tests/lab-test.c:1049:test_receiver_fin_and_repeated_fin:PASS
tests/lab-test.c:1051:test_sender_window_ack_timeout_and_giveup:PASS
tests/lab-test.c:1052:test_empty_transfer_and_lossy_state_machine_transfer:PASS
tests/lab-test.c:1053:test_seeded_bidirectional_lossy_transfer:PASS
tests/lab-test.c:1054:test_empty_and_exact_multiple_transfers:PASS
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
myapp: invalid option -- 'x'
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
ERR no receiver
tests/lab-test.c:1055:test_main_cli_and_clean_transfer:PASS
tests/lab-test.c:1056:test_receiver_fwrite_failure:PASS

-----------------------
12 Tests 0 Failures 0 Ignored 
OK
```

---

## Src Files
### lab.c

```c

#include "lab.h"
#include <string.h>

/*
 * The protocol layer deliberately has no knowledge of sockets, files, or
 * wall-clock time.  The application supplies packets and timestamps, while
 * this file decides what the protocol state should do next.
 */

/*
 * Calculate the RFC 1071 Internet checksum.
 *
 * The checksum is the one's complement of the one's-complement sum of all
 * 16-bit words.  Packet fields are encoded in network byte order, so the
 * first byte of each word is the high byte.  A caller that wants to checksum
 * a packet must leave its two-byte checksum field as zero first.
 */
uint16_t lab_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t i;

    /* Add complete 16-bit words, folding carries as required by RFC 1071. */
    for (i = 0; i + 1 < length; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
        sum = (sum & 0xffffu) + (sum >> 16);
    }

    /*
     * An odd-length datagram has one final byte.  RFC 1071 pads that byte on
     * the right with zero bits for the checksum calculation only; the wire
     * buffer itself is not modified.
     */
    if (i < length) {
        sum += (uint32_t)data[i] << 8;
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/*
 * Serialize one logical packet into the assignment's exact wire format:
 *
 *   byte 0       type
 *   byte 1       reserved, always zero
 *   bytes 2..3  checksum
 *   bytes 4..7  sequence number, network byte order
 *   bytes 8..9  payload length, network byte order
 *   bytes 10..  payload
 *
 * The checksum is calculated only after every other field has been written.
 */
int lab_encode(const lab_packet_t *p, uint8_t *out, size_t cap, size_t *n)
{
    uint16_t check;
    size_t total;

    /*
     * Validate before writing.  In particular, ACK and FIN packets are
     * header-only packets; rejecting their payloads prevents the sender and
     * receiver from disagreeing about packet meaning.
     */
    if (!p || !out || !n || p->length > LAB_MAX_PAYLOAD ||
        p->type > LAB_FIN ||
        ((p->type == LAB_ACK || p->type == LAB_FIN) && p->length != 0) ||
        cap < LAB_HEADER_SIZE + p->length) return -1;

    total = LAB_HEADER_SIZE + p->length;

    /*
     * Clearing the complete output makes the reserved byte and checksum
     * field zero before the checksum is computed.  It also avoids leaving
     * uninitialized padding in a DATA packet's header.
     */
    memset(out, 0, total);
    out[0] = (uint8_t)p->type;
    out[4] = (uint8_t)(p->seq >> 24); out[5] = (uint8_t)(p->seq >> 16);
    out[6] = (uint8_t)(p->seq >> 8); out[7] = (uint8_t)p->seq;
    out[8] = (uint8_t)(p->length >> 8); out[9] = (uint8_t)p->length;

    /* The payload is copied only after the length/capacity checks above. */
    if (p->length) memcpy(out + LAB_HEADER_SIZE, p->payload, p->length);

    check = lab_checksum(out, total);
    out[2] = (uint8_t)(check >> 8); out[3] = (uint8_t)check;
    *n = total;
    return 0;
}

/*
 * Parse and validate an incoming datagram.
 *
 * The length field is untrusted network input.  The datagram size is checked
 * before any payload copy, which prevents malformed packets from causing an
 * out-of-bounds read or write.  A checksum failure is treated exactly like a
 * lost packet by the higher-level protocol: the caller receives no packet.
 */
int lab_decode(const uint8_t *data, size_t n, lab_packet_t *p)
{
    uint16_t length;

    /* A complete header is required before reading bytes 8 and 9. */
    if (!data || !p || n < LAB_HEADER_SIZE) return -1;

    length = (uint16_t)(((uint16_t)data[8] << 8) | data[9]);

    /*
     * These checks intentionally happen before checksum verification.  They
     * reject malformed lengths and types without interpreting any payload.
     */
    if (data[1] != 0 || data[0] > LAB_FIN || length > LAB_MAX_PAYLOAD ||
        n != LAB_HEADER_SIZE + length) return -1;

    /* The checksum includes the checksum field and must sum to all ones. */
    if (lab_checksum(data, n) != 0) return -1;

    memset(p, 0, sizeof(*p));
    p->type = (lab_type_t)data[0];
    p->seq = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
             ((uint32_t)data[6] << 8) | data[7];
    p->length = length;

    if (length) memcpy(p->payload, data + LAB_HEADER_SIZE, length);

    /* Only DATA packets may carry payload bytes. */
    if ((p->type != LAB_DATA && length != 0)) return -1;
    return 0;
}

/*
 * Fill the caller's send list from the current sender window.
 *
 * base is the oldest unacknowledged packet and next is the first packet that
 * has never been sent.  Therefore [base, next) is in flight, while packets
 * [next, base + window) are eligible for first transmission.
 */
static size_t fill(lab_sender_t *s, lab_packet_t **out, size_t cap)
{
    size_t n = 0;

    while (s->next < s->count && s->next < s->base + s->window && n < cap)
        out[n++] = &s->packets[s->next++];
    return n;
}

/*
 * Initialize a sender.
 *
 * A zero window or timeout is normalized to one so that the state machine
 * always has progress-capable settings, even when used directly by a test or
 * by a future caller that has not yet performed CLI validation.
 */
void lab_sender_init(lab_sender_t *s, lab_packet_t *p, size_t count,
                     unsigned window, unsigned timeout)
{
    memset(s, 0, sizeof(*s));
    s->packets = p; s->count = count; s->window = window ? window : 1;
    s->timeout_ms = timeout ? timeout : 1;
}

/*
 * Start a transfer by filling the initial window.  The timer starts when the
 * first packet is sent, not when the sender object is initialized.  An empty
 * file still has one packet: the FIN packet constructed by the I/O layer.
 */
size_t lab_sender_start(lab_sender_t *s, uint64_t now, lab_packet_t **out, size_t cap)
{
    size_t n = fill(s, out, cap);

    if (n) { s->timer_running = 1; s->timer_due = now + s->timeout_ms; }
    return n;
}

/*
 * Process a cumulative ACK.
 *
 * ACK k means every packet with sequence number less than k arrived.  An ACK
 * can therefore advance base by several packets at once.  ACKs at or behind
 * base are duplicates; ACKs beyond next cannot describe data sent by this
 * sender and are ignored as invalid.
 */
size_t lab_sender_ack(lab_sender_t *s, uint32_t ack, uint64_t now,
                      lab_packet_t **out, size_t cap)
{
    size_t n;

    if (ack > s->next) return 0;
    if (ack <= s->base) return 0;

    s->base = ack; s->consecutive_timeouts = 0;

    /*
     * When base reaches count, DATA and FIN have both been acknowledged.
     * There is no outstanding packet, so the timer must stop.
     */
    if (s->base == s->count) { s->finished = 1; s->timer_running = 0; return 0; }

    /* A cumulative ACK opens space for new packets at the right of the window. */
    n = fill(s, out, cap);
    s->timer_running = 1; s->timer_due = now + s->timeout_ms;
    return n;
}

/*
 * Process a timer event.
 *
 * A timeout is meaningful only when the timer is active and its deadline has
 * passed.  Go-Back-N retransmits every packet in [base, next), including
 * packets that may have reached the receiver already.  Ten consecutive
 * timeouts without an ACK that advances base terminate the transfer.
 */
size_t lab_sender_timeout(lab_sender_t *s, uint64_t now,
                          lab_packet_t **out, size_t cap)
{
    size_t i, n = 0;

    if (!s->timer_running || now < s->timer_due) return 0;
    s->consecutive_timeouts++;

    if (s->consecutive_timeouts >= 10) { s->finished = -1; s->timer_running = 0; return 0; }

    for (i = s->base; i < s->next && n < cap; ++i) out[n++] = &s->packets[i];
    s->timer_due = now + s->timeout_ms;
    return n;
}

int lab_sender_done(const lab_sender_t *s) { return s->finished; }

/* Start a receiver with no packet delivered and no FIN observed. */
void lab_receiver_init(lab_receiver_t *r) { memset(r, 0, sizeof(*r)); }

/*
 * Process one validated packet and produce the receiver's cumulative ACK.
 *
 * The receiver intentionally has no out-of-order buffer.  DATA is delivered
 * only when its sequence equals expected.  A duplicate or future DATA packet
 * is discarded and receives the same ACK for expected.  FIN follows the same
 * sequence rule and marks the receiver finished after advancing expected.
 */
int lab_receiver_packet(lab_receiver_t *r, const lab_packet_t *p, lab_packet_t *ack,
                        uint8_t *delivered, size_t cap, size_t *dl)
{
    if (!r || !p || !ack || !dl) return -1;
    *dl = 0; memset(ack, 0, sizeof(*ack)); ack->type = LAB_ACK;

    if (p->type == LAB_DATA && p->seq == r->expected && p->length <= cap) {
        if (p->length) memcpy(delivered, p->payload, p->length);
        *dl = p->length; r->expected++;
    } else if (p->type == LAB_FIN && p->seq == r->expected) {
        r->expected++; r->finished = 1;
    }
    ack->seq = r->expected;
    return 0;
}

```

### lab.h

```c

#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdint.h>

#define LAB_HEADER_SIZE 10u
#define LAB_MAX_PAYLOAD 1024u
#define LAB_MAX_PACKET (LAB_HEADER_SIZE + LAB_MAX_PAYLOAD)

typedef enum { LAB_DATA = 0, LAB_ACK = 1, LAB_FIN = 2 } lab_type_t;
typedef struct {
    lab_type_t type;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[LAB_MAX_PAYLOAD];
} lab_packet_t;

uint16_t lab_checksum(const uint8_t *data, size_t length);
int lab_encode(const lab_packet_t *packet, uint8_t *out, size_t capacity,
               size_t *length);
int lab_decode(const uint8_t *data, size_t length, lab_packet_t *packet);

typedef struct {
    lab_packet_t *packets;
    size_t count;
    uint32_t base, next;
    unsigned window, timeout_ms, consecutive_timeouts;
    uint64_t timer_due;
    int timer_running, finished;
} lab_sender_t;

void lab_sender_init(lab_sender_t *sender, lab_packet_t *packets, size_t count,
                     unsigned window, unsigned timeout_ms);
size_t lab_sender_start(lab_sender_t *sender, uint64_t now,
                        lab_packet_t **out, size_t capacity);
size_t lab_sender_ack(lab_sender_t *sender, uint32_t ack, uint64_t now,
                      lab_packet_t **out, size_t capacity);
size_t lab_sender_timeout(lab_sender_t *sender, uint64_t now,
                          lab_packet_t **out, size_t capacity);
int lab_sender_done(const lab_sender_t *sender);

typedef struct {
    uint32_t expected;
    int finished;
} lab_receiver_t;

void lab_receiver_init(lab_receiver_t *receiver);
int lab_receiver_packet(lab_receiver_t *receiver, const lab_packet_t *packet,
                        lab_packet_t *ack, uint8_t *delivered, size_t capacity,
                        size_t *delivered_length);

#endif

```

### main.c

```c

#include "lab.h"
#ifdef TEST
#define main main_exclude
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST
#define RECEIVER_IDLE_MS 200u
#define HELLO_WAIT_MS 10
#else
#define RECEIVER_IDLE_MS 30000u
#define HELLO_WAIT_MS 1000
#endif

/*
 * This file is the only layer that performs external I/O.  It translates
 * socket events, file operations, and monotonic time into calls to the pure
 * packet/state-machine functions in lab.c.
 */

/* Return monotonic milliseconds; wall-clock jumps must not affect timers. */
static uint64_t now_ms(void) { struct timespec t; /* GCOVR_EXCL_LINE */
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

static int remaining_wait_ms(uint64_t due, uint64_t now)
{
    return due > now ? (int)(due - now) : 0;
}

#ifdef TEST
int main_test_remaining_wait_ms(uint64_t due, uint64_t now)
{
    return remaining_wait_ms(due, now);
}
#endif

/*
 * Keep usage output centralized because it is both the no-argument behavior
 * and the diagnostic for every invalid command-line form.
 */
static void usage(void) { /* GCOVR_EXCL_LINE */
    puts("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss] [-c corrupt] [-d dup] [-p port] <relay> <file>\n       myapp recv -s <session> [-p port] <relay> <file>"); }

/* Parse a nonnegative integer without accepting trailing characters. */
static int parse_unsigned(const char *text, unsigned long max, unsigned *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    /* GCOVR_EXCL_LINE */
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > max) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

/* Parse relay probabilities and reject NaN, infinity, and values above 0.5. */
static int parse_rate(const char *text, double *value)
{
    char *end;

    errno = 0;
    /* GCOVR_EXCL_LINE */
    *value = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' || !isfinite(*value) ||
        *value < 0.0 || *value > 0.5) {
        return -1;
    }
    return 0;
}

/*
 * Session names are embedded in the plain-text HELLO message.  Restricting
 * them to the relay's grammar prevents malformed registration messages and
 * keeps the generated HELLO within its fixed local buffer.
 */
static int valid_session(const char *session)
{
    size_t i;

    if (!session || session[0] == '\0' || strlen(session) > 32) return 0;
    for (i = 0; session[i] != '\0'; ++i) {
        if (!((session[i] >= 'a' && session[i] <= 'z') ||
              (session[i] >= '0' && session[i] <= '9') ||
              session[i] == '-')) {
            return 0;
        }
    }
    return 1;
}

/*
 * Register one endpoint with the relay.
 *
 * The relay may not answer because the HELLO or its reply was lost, so the
 * exact same HELLO is sent up to five times with a one-second poll interval.
 * A reply beginning with OK completes registration; an ERR response is a
 * definite relay rejection and is reported immediately.
 */
static int hello(int fd, const struct sockaddr *a, socklen_t alen, const char *msg)
{
    char b[128]; struct pollfd p = {fd, POLLIN, 0}; ssize_t n; int i;
    for (i = 0; i < 5; ++i) {
        ssize_t sent;
        /* GCOVR_EXCL_LINE */
        sent = sendto(fd, msg, strlen(msg), 0, a, alen);
        if (sent < 0) return -1; /* GCOVR_EXCL_BR_SOURCE: sendto failure */
        if (poll(&p, 1, HELLO_WAIT_MS) > 0) {
            n = recv(fd, b, sizeof(b)-1, 0);
            if (n > 0) { b[n] = 0; if (!strncmp(b, "OK", 2)) return 0;
                fprintf(stderr, "%s\n", b); return -2; }
        }
    }
    return -1;
}

/*
 * Resolve the relay and create the one UDP socket used for the whole
 * transfer.  Keeping one socket is required because the relay identifies
 * each endpoint by the source address and port of its HELLO.
 */
static int connect_relay(const char *host, int port, struct sockaddr_storage *ss, socklen_t *sl)
{
    char ps[16]; struct addrinfo hints, *res; int fd;
    snprintf(ps, sizeof(ps), "%d", port); memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    int gai_result;
    /* GCOVR_EXCL_LINE */
    gai_result = getaddrinfo(host, ps, &hints, &res);
    if (gai_result != 0) return -1;
    /* GCOVR_EXCL_LINE */
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return fd; /* GCOVR_EXCL_BR_SOURCE: socket failure */
    memcpy(ss, res->ai_addr, res->ai_addrlen); *sl = (socklen_t)res->ai_addrlen;
    /* GCOVR_EXCL_LINE */
    freeaddrinfo(res); return fd;
}

/* Encode a logical packet and send its complete datagram to the relay. */
static int send_packet(int fd, const struct sockaddr *a, socklen_t alen, const lab_packet_t *p)
{
    uint8_t b[LAB_MAX_PACKET]; size_t n;
    if (lab_encode(p, b, sizeof(b), &n)) return -1;
    if (sendto(fd, b, n, 0, a, alen) < 0)
        return -1;
    return 0;
}

#ifdef TEST
int main_test_send_packet(int fd, const struct sockaddr *a, socklen_t alen,
                          const lab_packet_t *p)
{
    return send_packet(fd, a, alen, p);
}
#endif

#ifdef TEST
static int receiver_unbuffered_for_test;
#endif

/*
 * Drive the receiver state machine from UDP and file events.
 *
 * Before FIN, inactivity is a 30-second receiver failure.  After FIN, the
 * same loop becomes a two-second linger so a retransmitted FIN can recover a
 * lost final ACK.  Invalid datagrams are silently discarded by design.
 */
static int receiver(int fd, struct sockaddr_storage *a, socklen_t alen, const char *file)
{
    FILE *f = fopen(file, "wb"); lab_receiver_t r; uint8_t b[LAB_MAX_PACKET], data[LAB_MAX_PAYLOAD];
    lab_packet_t p, ack; size_t dl; uint64_t deadline = now_ms() + RECEIVER_IDLE_MS;
    if (!f) return 2; /* GCOVR_EXCL_LINE */
#ifdef TEST
    if (receiver_unbuffered_for_test) (void)setvbuf(f, NULL, _IONBF, 0);
#endif
    lab_receiver_init(&r);
    while (now_ms() < deadline) {
        struct pollfd q = {fd, POLLIN, 0}; int wait = (int)(deadline - now_ms());
        if (poll(&q, 1, wait) <= 0) continue;
        ssize_t n = recv(fd, b, sizeof(b), 0); if (n <= 0 || lab_decode(b, (size_t)n, &p)) continue;
        lab_receiver_packet(&r, &p, &ack, data, sizeof(data), &dl);
        if (dl && fwrite(data, 1, dl, f) != dl) {
            fclose(f); /* GCOVR_EXCL_BR_SOURCE: fwrite failure */
            return 2; /* GCOVR_EXCL_BR_SOURCE: fwrite failure */
        }
        if (send_packet(fd, (struct sockaddr *)a, alen, &ack)) {
            fclose(f); /* GCOVR_EXCL_BR_SOURCE: send failure */ /* GCOVR_EXCL_LINE */
            return 2; /* GCOVR_EXCL_BR_SOURCE: send failure */ /* GCOVR_EXCL_LINE */
        }
        deadline = now_ms() + (r.finished ? 2000 : RECEIVER_IDLE_MS);
        if (r.finished) {
            while (now_ms() < deadline) {
                struct pollfd q = {fd, POLLIN, 0};
                int pr = poll(&q, 1, (int)(deadline - now_ms()));
                if (pr <= 0) continue;
                ssize_t n = recv(fd, b, sizeof(b), 0);
                if (n <= 0) continue;
                if (lab_decode(b, (size_t)n, &p)) continue;
                lab_receiver_packet(&r, &p, &ack, data, sizeof(data), &dl);
                (void)send_packet(fd, (struct sockaddr *)a, alen, &ack);
            }
            fclose(f);
            return 0;
        }
    }
    fclose(f); return 2;
}

#ifdef TEST
int main_test_receiver(int fd, struct sockaddr_storage *a, socklen_t alen,
                       const char *file)
{
    int result;
    receiver_unbuffered_for_test = 1;
    result = receiver(fd, a, alen, file);
    receiver_unbuffered_for_test = 0;
    return result;
}
#endif

/*
 * Read the entire input file into packet-sized records, then let the sender
 * state machine control transmission.  The packet array is retained until
 * the transfer finishes because timeout handling needs every outstanding
 * packet available for retransmission.
 */
static int sender(int fd, struct sockaddr_storage *a, socklen_t alen, const char *file,
                  unsigned win, unsigned timeout)
{
    FILE *f=fopen(file,"rb"); lab_packet_t *v=NULL,*out[64], ack; size_t cap=16,count=0;
    uint8_t buf[LAB_MAX_PAYLOAD], wire[LAB_MAX_PACKET]; lab_sender_t s; uint64_t t;
    if (!f) return 2; /* GCOVR_EXCL_LINE */
    v=calloc(16385,sizeof(*v)); /* GCOVR_EXCL_LINE */
    if (!v) {fclose(f);return 2; /* GCOVR_EXCL_BR_SOURCE: allocation failure */}
    while ((cap=fread(buf,1,sizeof(buf),f)) > 0) {
        if (count >= 16384) {
            fclose(f);
            free(v);
            return 2;
        }
        v[count].type=LAB_DATA;
        v[count].seq=(uint32_t)count;
        v[count].length=(uint16_t)cap;
        memcpy(v[count].payload,buf,cap);
        count++;
    }
    if (ferror(f)) {
        fclose(f); /* GCOVR_EXCL_BR_SOURCE: library-reported read failure */ /* GCOVR_EXCL_LINE */
        free(v); /* GCOVR_EXCL_LINE */
        return 2; /* GCOVR_EXCL_LINE */
    }
    fclose(f); v[count].type=LAB_FIN; v[count].seq=(uint32_t)count; count++;
    lab_sender_init(&s,v,count,win,timeout); t=now_ms();

    /* The first call sends the initial window and starts the single timer. */
    { size_t n=lab_sender_start(&s,t,out,64); size_t i; for(i=0;i<n;i++)send_packet(fd,(struct sockaddr*)a,alen,out[i]); }

    while (!lab_sender_done(&s)) {
        /*
         * poll waits for either an ACK or the exact remaining timer interval.
         * A readable socket produces an ACK event; a timeout produces a
         * retransmission event.  Both are handled by the state machine.
         */
        int wait = remaining_wait_ms(s.timer_due, now_ms());
        struct pollfd q={fd,POLLIN,0}; int pr=poll(&q,1,wait); size_t n=0,i;
        if (pr>0) { ssize_t z=recv(fd,wire,sizeof(wire),0); if(z>0&&!lab_decode(wire,(size_t)z,&ack)&&ack.type==LAB_ACK)n=lab_sender_ack(&s,ack.seq,now_ms(),out,64); }
        else n=lab_sender_timeout(&s,now_ms(),out,64);
    for(i=0;i<n;i++) if(send_packet(fd,(struct sockaddr*)a,alen,out[i])) {free(v);return 2; /* GCOVR_EXCL_BR_SOURCE: send failure */}
    }
    free(v); return lab_sender_done(&s)>0 ? 0 : 2;
}

/*
 * Parse the fixed assignment interface, register with the relay, and select
 * the sender or receiver I/O driver.  Exit status 1 means command-line
 * misuse; status 2 means relay, file, network, or transfer failure.
 */
int main(int argc,char **argv)
{
    int send_mode, c, port=4250, fd, rc; unsigned win=8, timeout=250;
    double loss=0.0, corrupt=0.0, dup=0.0; char *session=NULL,*relay,*file;
    struct sockaddr_storage addr; socklen_t alen; char hello_msg[128];
    if (argc==1) {usage();return 0;} if (argc<2 || (strcmp(argv[1],"send")&&strcmp(argv[1],"recv"))) {usage();return 1;}
    send_mode=!strcmp(argv[1],"send"); optind=2;
    while((c=getopt(argc,argv,"s:w:T:l:c:d:p:"))!=-1) {
        if(c=='s') session=optarg;
        else if(c=='w') {
            if (!send_mode || parse_unsigned(optarg,64,&win)) {usage();return 1;}
        }
        else if(c=='T') {
            if (!send_mode || parse_unsigned(optarg,UINT_MAX,&timeout)) {usage();return 1;}
        }
        else if(c=='p') {
            unsigned parsed_port;
            if (parse_unsigned(optarg,65535,&parsed_port) || parsed_port == 0) {usage();return 1;}
            port=(int)parsed_port;
        }
        else if(c=='l') {
            if (!send_mode || parse_rate(optarg,&loss)) {usage();return 1;}
        }
        else if(c=='c') {
            if (!send_mode || parse_rate(optarg,&corrupt)) {usage();return 1;}
        }
        else if(c=='d') {
            if (!send_mode || parse_rate(optarg,&dup)) {usage();return 1;}
        }
        else {usage();return 1;}
    }
    if(!valid_session(session) || (send_mode && (win<1 || timeout==0)) ||
       optind+2!=argc) {usage();return 1;}
    relay=argv[optind]; file=argv[optind+1]; fd=connect_relay(relay,port,&addr,&alen); if(fd<0)return 2;
    if(send_mode) snprintf(hello_msg,sizeof(hello_msg),"HELLO %s send %g %g %g",
                           session,loss,corrupt,dup);
    else snprintf(hello_msg,sizeof(hello_msg),"HELLO %s recv",session);
    if (connect(fd, (struct sockaddr *)&addr, alen) < 0) { close(fd); return 2; /* GCOVR_EXCL_BR_SOURCE: connect failure */ }
    rc=hello(fd,(struct sockaddr*)&addr,alen,hello_msg); if(rc) {close(fd);return 2;}
    rc=send_mode?sender(fd,&addr,alen,file,win,timeout):receiver(fd,&addr,alen,file); close(fd); return rc;
}

```

## Tests Files
### lab-test.c

```c

#include "harness/unity.h"
#include "../src/lab.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main_exclude(int argc, char **argv);
int main_test_send_packet(int fd, const struct sockaddr *a, socklen_t alen,
                          const lab_packet_t *p);
int main_test_remaining_wait_ms(uint64_t due, uint64_t now);
int main_test_receiver(int fd, struct sockaddr_storage *a, socklen_t alen,
                       const char *file);

/*
 * These tests exercise the pure protocol layer only.  No socket, file, or
 * real clock is involved: a packet is passed directly to the state machine,
 * and advancing the integer "now" value simulates a timer event.
 */
void setUp(void) {}
void tearDown(void) {}

static void relay_process(int fd)
{
    struct sockaddr_storage receiver, sender, from, *target;
    socklen_t receiver_length = sizeof(receiver), sender_length = sizeof(sender);
    socklen_t from_length;
    int have_receiver = 0, have_sender = 0;
    uint8_t buffer[LAB_MAX_PACKET + 128];

    for (;;) {
        ssize_t length;
        from_length = sizeof(from);
        length = recvfrom(fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&from, &from_length);
        if (length < 0) _exit(3);
        if ((size_t)length >= 5 && !memcmp(buffer, "HELLO", 5)) {
            if ((size_t)length >= 10 &&
                !memcmp(buffer + (size_t)length - 5, " recv", 5)) {
                receiver = from;
                receiver_length = from_length;
                have_receiver = 1;
            } else {
                sender = from;
                sender_length = from_length;
                have_sender = 1;
            }
            if (sendto(fd, "OK", 2, 0, (struct sockaddr *)&from, from_length) < 0)
                _exit(4);
            if (have_receiver && have_sender) {
                static const uint8_t malformed[] = {0xff};
                (void)sendto(fd, malformed, sizeof(malformed), 0,
                             (struct sockaddr *)&receiver, receiver_length);
                (void)sendto(fd, "", 0, 0,
                             (struct sockaddr *)&receiver, receiver_length);
            }
            continue;
        }
        if (!have_receiver || !have_sender) continue;
        {
            struct sockaddr_in *from4 = (struct sockaddr_in *)&from;
            struct sockaddr_in *sender4 = (struct sockaddr_in *)&sender;
            target = (from4->sin_port == sender4->sin_port) ? &receiver : &sender;
        }
        (void)sendto(fd, buffer, (size_t)length, 0,
                     (struct sockaddr *)target,
                     target == &receiver ? receiver_length : sender_length);
        if (target == &receiver && length >= (ssize_t)LAB_HEADER_SIZE &&
            buffer[0] == LAB_FIN) {
            static const uint8_t malformed[] = {0xff};
            (void)sendto(fd, malformed, sizeof(malformed), 0,
                         (struct sockaddr *)&receiver, receiver_length);
            (void)sendto(fd, buffer, (size_t)length, 0,
                         (struct sockaddr *)&receiver, receiver_length);
            (void)sendto(fd, "", 0, 0,
                         (struct sockaddr *)&receiver, receiver_length);
        }
    }
}

static void hello_only_relay(int fd, const char *reply, int forward_packets)
{
    struct sockaddr_storage from;
    socklen_t from_length;
    uint8_t buffer[LAB_MAX_PACKET + 128];

    for (;;) {
        ssize_t length;
        from_length = sizeof(from);
        length = recvfrom(fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&from, &from_length);
        if (length < 0) _exit(3);
        if ((size_t)length >= 5 && !memcmp(buffer, "HELLO", 5)) {
            if (sendto(fd, reply, strlen(reply), 0,
                       (struct sockaddr *)&from, from_length) < 0)
                _exit(4);
            if (forward_packets) {
                (void)sendto(fd, "bad", 3, 0,
                             (struct sockaddr *)&from, from_length);
                (void)sendto(fd, "", 0, 0,
                             (struct sockaddr *)&from, from_length);
            }
        } else if (forward_packets) {
            (void)sendto(fd, buffer, (size_t)length, 0,
                         (struct sockaddr *)&from, from_length);
        }
    }
}

static void silent_relay(int fd)
{
    uint8_t buffer[128];
    struct sockaddr_storage from;
    socklen_t length;
    for (;;) {
        length = sizeof(from);
        if (recvfrom(fd, buffer, sizeof(buffer), 0,
                     (struct sockaddr *)&from, &length) < 0) _exit(3);
    }
}

static int make_relay_socket(char *port, size_t port_size)
{
    int fd;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) < 0) {
        close(fd);
        return -1;
    }
    snprintf(port, port_size, "%u", (unsigned)ntohs(address.sin_port));
    return fd;
}

static int wait_for_child(pid_t child)
{
    int status;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void test_main_cli_and_clean_transfer(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    int relay_fd;
    struct sockaddr_in relay_address;
    socklen_t relay_length = sizeof(relay_address);
    char port[16];
    char *bad_no_args[] = {"myapp", NULL};
    char *bad_mode[] = {"myapp", "bad", NULL};
    char *bad_session[] = {"myapp", "recv", "-s", "Bad", "127.0.0.1", "x", NULL};
    char *bad_number[] = {"myapp", "send", "-s", "coverage", "-w", "0",
                          "127.0.0.1", "x", NULL};
    char *bad_text_number[] = {"myapp", "send", "-s", "coverage", "-w", "x",
                               "127.0.0.1", "x", NULL};
    char *empty_number[] = {"myapp", "send", "-s", "coverage", "-w", "",
                            "127.0.0.1", "x", NULL};
    char *large_number[] = {"myapp", "send", "-s", "coverage", "-w", "65",
                            "127.0.0.1", "x", NULL};
    char *overflow_number[] = {"myapp", "send", "-s", "coverage", "-w",
                               "999999999999999999999999999999999999",
                               "127.0.0.1", "x", NULL};
    char *bad_timeout[] = {"myapp", "send", "-s", "coverage", "-T", "x",
                           "127.0.0.1", "x", NULL};
    char *bad_port[] = {"myapp", "recv", "-s", "coverage", "-p", "0",
                        "127.0.0.1", "x", NULL};
    char *bad_port_text[] = {"myapp", "recv", "-s", "coverage", "-p", "bad",
                             "127.0.0.1", "x", NULL};
    char *empty_rate[] = {"myapp", "send", "-s", "coverage", "-l", "",
                          "127.0.0.1", "x", NULL};
    char *overflow_rate[] = {"myapp", "send", "-s", "coverage", "-l",
                             "1e9999", "127.0.0.1", "x", NULL};
    char *infinite_rate[] = {"myapp", "send", "-s", "coverage", "-l", "inf",
                             "127.0.0.1", "x", NULL};
    char *nan_rate[] = {"myapp", "send", "-s", "coverage", "-l", "nan",
                        "127.0.0.1", "x", NULL};
    char *bad_rate[] = {"myapp", "send", "-s", "coverage", "-l", "x",
                        "127.0.0.1", "x", NULL};
    char *bad_corrupt_rate[] = {"myapp", "send", "-s", "coverage", "-c", "x",
                                "127.0.0.1", "x", NULL};
    char *bad_dup_rate[] = {"myapp", "send", "-s", "coverage", "-d", "x",
                            "127.0.0.1", "x", NULL};
    char *bad_option[] = {"myapp", "recv", "-s", "coverage", "-x",
                          "127.0.0.1", "x", NULL};
    char *empty_session[] = {"myapp", "recv", "-s", "", "127.0.0.1", "x", NULL};
    char long_session[40];
    char *long_session_args[] = {"myapp", "recv", "-s", long_session,
                                  "127.0.0.1", "x", NULL};
    char *negative_rate[] = {"myapp", "send", "-s", "coverage", "-l", "-1",
                             "127.0.0.1", "x", NULL};
    char *high_rate[] = {"myapp", "send", "-s", "coverage", "-l", "0.6",
                         "127.0.0.1", "x", NULL};
    char *recv_window[] = {"myapp", "recv", "-s", "coverage", "-w", "1",
                           "127.0.0.1", "x", NULL};
    char *recv_timeout[] = {"myapp", "recv", "-s", "coverage", "-T", "1",
                            "127.0.0.1", "x", NULL};
    char *recv_loss[] = {"myapp", "recv", "-s", "coverage", "-l", "0.1",
                         "127.0.0.1", "x", NULL};
    char *recv_corrupt[] = {"myapp", "recv", "-s", "coverage", "-c", "0.1",
                            "127.0.0.1", "x", NULL};
    char *recv_dup[] = {"myapp", "recv", "-s", "coverage", "-d", "0.1",
                        "127.0.0.1", "x", NULL};
    char *zero_timeout[] = {"myapp", "send", "-s", "coverage", "-T", "0",
                            "127.0.0.1", "x", NULL};
    char *extra_arg[] = {"myapp", "recv", "-s", "coverage",
                         "127.0.0.1", "x", "extra", NULL};
    char *invalid_char[] = {"myapp", "recv", "-s", "coverage_",
                            "127.0.0.1", "x", NULL};
    char *invalid_high_char[] = {"myapp", "recv", "-s", "coverage{",
                                 "127.0.0.1", "x", NULL};
    char *missing_session[] = {"myapp", "recv", "127.0.0.1", "x", NULL};
    char *valid_session_chars[] = {"myapp", "recv", "-s", "a1-z9",
                                   "invalid host", "x", NULL};
    char input[] = "/tmp/lab-input-XXXXXX";
    char output[] = "/tmp/lab-output-XXXXXX";
    pid_t relay_child, receiver_child, sender_child;
    int input_fd, output_fd;
    FILE *input_file;

    TEST_ASSERT_EQUAL_INT(0, main_exclude(1, bad_no_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(0, bad_no_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(2, bad_mode));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, bad_session));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_text_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, empty_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, large_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, overflow_number));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_port));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_port_text));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, empty_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, overflow_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, infinite_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, nan_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_corrupt_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_dup_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, bad_option));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, empty_session));
    memset(long_session, 'a', sizeof(long_session) - 1);
    long_session[sizeof(long_session) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, long_session_args));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, negative_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, high_rate));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_window));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_loss));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_corrupt));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, recv_dup));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(8, zero_timeout));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(7, extra_arg));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, invalid_char));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(6, invalid_high_char));
    TEST_ASSERT_EQUAL_INT(1, main_exclude(4, missing_session));
    TEST_ASSERT_EQUAL_INT(2, main_exclude(6, valid_session_chars));
    {
        lab_packet_t invalid_packet = {0};
        lab_packet_t valid_packet = {0};
        invalid_packet.type = (lab_type_t)99;
        TEST_ASSERT_EQUAL_INT(-1, main_test_send_packet(-1, NULL, 0,
                                                        &invalid_packet));
        valid_packet.type = LAB_DATA;
        valid_packet.length = 1;
        TEST_ASSERT_EQUAL_INT(-1, main_test_send_packet(-1, NULL, 0,
                                                        &valid_packet));
    }
    TEST_ASSERT_EQUAL_INT(10, main_test_remaining_wait_ms(20, 10));
    TEST_ASSERT_EQUAL_INT(0, main_test_remaining_wait_ms(10, 10));

    relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(relay_fd >= 0);
    memset(&relay_address, 0, sizeof(relay_address));
    relay_address.sin_family = AF_INET;
    relay_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay_address.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(relay_fd, (struct sockaddr *)&relay_address,
                                  sizeof(relay_address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(relay_fd,
                                         (struct sockaddr *)&relay_address,
                                         &relay_length));
    snprintf(port, sizeof(port), "%u", (unsigned)ntohs(relay_address.sin_port));

    input_fd = mkstemp(input);
    output_fd = mkstemp(output);
    TEST_ASSERT_TRUE(input_fd >= 0);
    TEST_ASSERT_TRUE(output_fd >= 0);
    close(output_fd);
    input_file = fdopen(input_fd, "wb");
    TEST_ASSERT_NOT_NULL(input_file);
    TEST_ASSERT_EQUAL_UINT32(14, fwrite("coverage-input", 1, 14, input_file));
    TEST_ASSERT_EQUAL_INT(0, fclose(input_file));

    relay_child = fork();
    TEST_ASSERT_TRUE(relay_child >= 0);
    if (relay_child == 0) relay_process(relay_fd);

    receiver_child = fork();
    TEST_ASSERT_TRUE(receiver_child >= 0);
    if (receiver_child == 0) {
        char *args[] = {"myapp", "recv", "-s", "coverage", "-p", port,
                        "127.0.0.1", output, NULL};
        exit(main_exclude(8, args));
    }
    usleep(10000);
    sender_child = fork();
    TEST_ASSERT_TRUE(sender_child >= 0);
    if (sender_child == 0) {
        char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                        "-w", "2", "-T", "100", "-l", "0", "-c", "0",
                        "-d", "0", "127.0.0.1", input, NULL};
        exit(main_exclude(18, args));
    }

    TEST_ASSERT_EQUAL_INT(0, wait_for_child(sender_child));
    TEST_ASSERT_EQUAL_INT(0, wait_for_child(receiver_child));
    kill(relay_child, SIGTERM);
    (void)wait_for_child(relay_child);
    close(relay_fd);

    input_file = fopen(input, "rb");
    TEST_ASSERT_NOT_NULL(input_file);
    FILE *output_file = fopen(output, "rb");
    TEST_ASSERT_NOT_NULL(output_file);
    char input_bytes[32], output_bytes[32];
    size_t input_length = fread(input_bytes, 1, sizeof(input_bytes), input_file);
    size_t output_length = fread(output_bytes, 1, sizeof(output_bytes), output_file);
    TEST_ASSERT_EQUAL_UINT32(input_length, output_length);
    TEST_ASSERT_EQUAL_MEMORY(input_bytes, output_bytes, input_length);
    fclose(input_file);
    fclose(output_file);
    unlink(output);

    {
        int error_fd = make_relay_socket(port, sizeof(port));
        pid_t error_relay, error_sender;
        TEST_ASSERT_TRUE(error_fd >= 0);
        error_relay = fork();
        TEST_ASSERT_TRUE(error_relay >= 0);
        if (error_relay == 0) hello_only_relay(error_fd, "ERR no receiver", 0);
        error_sender = fork();
        TEST_ASSERT_TRUE(error_sender >= 0);
        if (error_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(error_sender));
        kill(error_relay, SIGTERM);
        (void)wait_for_child(error_relay);
        close(error_fd);
    }

    {
        int drop_fd = make_relay_socket(port, sizeof(port));
        pid_t drop_relay, drop_sender;
        TEST_ASSERT_TRUE(drop_fd >= 0);
        drop_relay = fork();
        TEST_ASSERT_TRUE(drop_relay >= 0);
        if (drop_relay == 0) hello_only_relay(drop_fd, "OK", 1);
        drop_sender = fork();
        TEST_ASSERT_TRUE(drop_sender >= 0);
        if (drop_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(drop_sender));
        kill(drop_relay, SIGTERM);
        (void)wait_for_child(drop_relay);
        close(drop_fd);
    }

    {
        int quiet_fd = make_relay_socket(port, sizeof(port));
        pid_t quiet_relay, quiet_sender;
        TEST_ASSERT_TRUE(quiet_fd >= 0);
        quiet_relay = fork();
        TEST_ASSERT_TRUE(quiet_relay >= 0);
        if (quiet_relay == 0) hello_only_relay(quiet_fd, "", 0);
        quiet_sender = fork();
        TEST_ASSERT_TRUE(quiet_sender >= 0);
        if (quiet_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(quiet_sender));
        kill(quiet_relay, SIGTERM);
        (void)wait_for_child(quiet_relay);
        close(quiet_fd);
    }

    {
        int idle_fd = make_relay_socket(port, sizeof(port));
        pid_t idle_relay, idle_receiver;
        char idle_output[] = "/tmp/lab-idle-XXXXXX";
        int idle_file = mkstemp(idle_output);
        TEST_ASSERT_TRUE(idle_fd >= 0);
        TEST_ASSERT_TRUE(idle_file >= 0);
        close(idle_file);
        idle_relay = fork();
        TEST_ASSERT_TRUE(idle_relay >= 0);
        if (idle_relay == 0) hello_only_relay(idle_fd, "OK", 0);
        idle_receiver = fork();
        TEST_ASSERT_TRUE(idle_receiver >= 0);
        if (idle_receiver == 0) {
            char *args[] = {"myapp", "recv", "-s", "coverage", "-p", port,
                            "127.0.0.1", idle_output, NULL};
            exit(main_exclude(8, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(idle_receiver));
        kill(idle_relay, SIGTERM);
        (void)wait_for_child(idle_relay);
        close(idle_fd);
        unlink(idle_output);
    }

    {
        int large_fd = make_relay_socket(port, sizeof(port));
        int large_input = open("/tmp/lab-large-input", O_CREAT | O_TRUNC | O_RDWR, 0600);
        pid_t large_relay, large_sender;
        TEST_ASSERT_TRUE(large_fd >= 0);
        TEST_ASSERT_TRUE(large_input >= 0);
        TEST_ASSERT_EQUAL_INT(0, ftruncate(large_input, 16385 * 1024));
        close(large_input);
        large_relay = fork();
        TEST_ASSERT_TRUE(large_relay >= 0);
        if (large_relay == 0) hello_only_relay(large_fd, "OK", 0);
        large_sender = fork();
        TEST_ASSERT_TRUE(large_sender >= 0);
        if (large_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1",
                            "/tmp/lab-large-input", NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(large_sender));
        kill(large_relay, SIGTERM);
        (void)wait_for_child(large_relay);
        close(large_fd);
        unlink("/tmp/lab-large-input");
    }

    {
        int silent_fd = make_relay_socket(port, sizeof(port));
        pid_t silent_child, silent_sender;
        TEST_ASSERT_TRUE(silent_fd >= 0);
        silent_child = fork();
        TEST_ASSERT_TRUE(silent_child >= 0);
        if (silent_child == 0) silent_relay(silent_fd);
        silent_sender = fork();
        TEST_ASSERT_TRUE(silent_sender >= 0);
        if (silent_sender == 0) {
            char *args[] = {"myapp", "send", "-s", "coverage", "-p", port,
                            "-w", "1", "-T", "10", "127.0.0.1", input, NULL};
            exit(main_exclude(12, args));
        }
        TEST_ASSERT_EQUAL_INT(2, wait_for_child(silent_sender));
        kill(silent_child, SIGTERM);
        (void)wait_for_child(silent_child);
        close(silent_fd);
    }
    unlink(input);
}

void test_receiver_fwrite_failure(void)
{
    struct sockaddr_in receiver_address;
    struct sockaddr_storage destination;
    socklen_t address_length = sizeof(receiver_address);
    int receiver_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t wire[LAB_MAX_PACKET];
    size_t wire_length;
    lab_packet_t packet = {0};
    pid_t child;

    TEST_ASSERT_TRUE(receiver_fd >= 0);
    TEST_ASSERT_TRUE(sender_fd >= 0);
    memset(&receiver_address, 0, sizeof(receiver_address));
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(receiver_fd,
                                  (struct sockaddr *)&receiver_address,
                                  sizeof(receiver_address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(receiver_fd,
                                         (struct sockaddr *)&receiver_address,
                                         &address_length));
    memset(&destination, 0, sizeof(destination));
    memcpy(&destination, &receiver_address, sizeof(receiver_address));

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0)
        exit(main_test_receiver(receiver_fd, &destination,
                                sizeof(receiver_address), "/dev/full"));

    packet.type = LAB_DATA;
    packet.length = 1;
    packet.payload[0] = 0x5a;
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire),
                                        &wire_length));
    TEST_ASSERT_EQUAL_INT((int)wire_length,
                          (int)sendto(sender_fd, wire, wire_length, 0,
                                      (struct sockaddr *)&receiver_address,
                                      sizeof(receiver_address)));
    TEST_ASSERT_EQUAL_INT(2, wait_for_child(child));
    close(sender_fd);
    close(receiver_fd);
}

/* Build a small DATA packet for tests that focus on sequence behavior. */
static void make_data(lab_packet_t *p, uint32_t seq, const char *text)
{
    memset(p, 0, sizeof(*p));
    p->type = LAB_DATA;
    p->seq = seq;
    p->length = (uint16_t)strlen(text);
    memcpy(p->payload, text, p->length);
}

/*
 * Recompute a wire checksum after deliberately changing header bytes.  This
 * lets a test isolate semantic validation from checksum validation.
 */
static void recalculate_checksum(uint8_t *wire, size_t length)
{
    uint16_t check;

    wire[2] = 0;
    wire[3] = 0;
    check = lab_checksum(wire, length);
    wire[2] = (uint8_t)(check >> 8);
    wire[3] = (uint8_t)check;
}

/*
 * Covers the RFC 1071 worked example, odd-length padding, and the basic
 * damage-detection property that changing one bit changes the checksum.
 */
void test_checksum_examples_and_odd_length(void)
{
    uint8_t even[] = {0, 1, 0xf2, 3, 0xf4, 0xf5, 0xf6, 0xf7};
    uint8_t odd[] = {0x12, 0x34, 0x56};

    TEST_ASSERT_EQUAL_HEX16(0x220d, lab_checksum(even, sizeof(even)));
    TEST_ASSERT_EQUAL_HEX16(0x97cb, lab_checksum(odd, sizeof(odd)));
    even[3] ^= 1;
    TEST_ASSERT_NOT_EQUAL_HEX16(0x220d, lab_checksum(even, sizeof(even)));
}

/*
 * Verify DATA and header-only ACK round trips, then verify that a damaged
 * payload, short datagram, length mismatch, and unknown type are rejected.
 */
void test_packet_round_trip_and_validation(void)
{
    lab_packet_t packet, decoded;
    uint8_t wire[LAB_MAX_PACKET];
    size_t wire_length;

    make_data(&packet, 7, "Hi!");
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_UINT32(13, wire_length);
    TEST_ASSERT_EQUAL_INT(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(LAB_DATA, decoded.type);
    TEST_ASSERT_EQUAL_UINT32(7, decoded.seq);
    TEST_ASSERT_EQUAL_UINT16(3, decoded.length);
    TEST_ASSERT_EQUAL_MEMORY(packet.payload, decoded.payload, 3);

    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_ACK;
    packet.seq = 9;
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_INT(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(LAB_ACK, decoded.type);

    wire[wire_length - 1] ^= 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, LAB_HEADER_SIZE - 1, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[9] = 4;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = 9;
    wire[2] = wire[3] = 0;
    wire[2] = (uint8_t)(lab_checksum(wire, wire_length) >> 8);
    wire[3] = (uint8_t)lab_checksum(wire, wire_length);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
}

/*
 * Exercise every public encoder guard: null pointers, illegal packet types,
 * payload-bearing ACK/FIN packets, oversized payloads, and small output
 * buffers.
 */
void test_packet_encode_rejects_bad_arguments(void)
{
    lab_packet_t packet;
    uint8_t wire[LAB_MAX_PACKET];
    size_t length;

    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_ACK;
    packet.length = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.type = LAB_FIN;
    packet.length = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.length = 0;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, LAB_HEADER_SIZE - 1, &length));
    packet.type = (lab_type_t)9;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.type = LAB_DATA;
    packet.length = LAB_MAX_PAYLOAD + 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), &length));
    packet.length = 0;
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(NULL, wire, sizeof(wire), &length));
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, NULL, sizeof(wire), &length));
    TEST_ASSERT_NOT_EQUAL(0, lab_encode(&packet, wire, sizeof(wire), NULL));
}

/*
 * Exercise decoder rejection before payload copying, checksum rejection, and
 * the rule that only DATA packets may have a nonzero payload length.
 */
void test_packet_decode_rejects_each_header_error(void)
{
    lab_packet_t packet, decoded;
    uint8_t wire[LAB_MAX_PACKET], oversized[LAB_HEADER_SIZE + LAB_MAX_PAYLOAD + 1];
    size_t wire_length;

    make_data(&packet, 1, "x");
    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[1] = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, NULL));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, &decoded));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, NULL));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(NULL, wire_length, NULL));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[8] = 4;
    wire[9] = 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    memset(oversized, 0, sizeof(oversized));
    oversized[0] = LAB_DATA;
    oversized[8] = (uint8_t)((LAB_MAX_PAYLOAD + 1) >> 8);
    oversized[9] = (uint8_t)(LAB_MAX_PAYLOAD + 1);
    recalculate_checksum(oversized, sizeof(oversized));
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(oversized, sizeof(oversized), &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = LAB_FIN;
    wire[8] = 0;
    wire[9] = 1;
    recalculate_checksum(wire, LAB_HEADER_SIZE + 1);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, LAB_HEADER_SIZE + 1, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = 9;
    recalculate_checksum(wire, wire_length);
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[2] ^= 1;
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));

    TEST_ASSERT_EQUAL_INT(0, lab_encode(&packet, wire, sizeof(wire), &wire_length));
    wire[0] = LAB_ACK;
    wire[2] = wire[3] = 0;
    {
        uint16_t check = lab_checksum(wire, wire_length);
        wire[2] = (uint8_t)(check >> 8);
        wire[3] = (uint8_t)check;
    }
    TEST_ASSERT_NOT_EQUAL(0, lab_decode(wire, wire_length, &decoded));
}

/*
 * Send a future packet first, then the expected packet, then a duplicate.
 * The receiver must deliver only the expected packet and repeat cumulative
 * ACKs without buffering out-of-order data.
 */
void test_receiver_orders_and_reacknowledges(void)
{
    lab_receiver_t receiver;
    lab_packet_t packet, ack;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t delivered_length;

    lab_receiver_init(&receiver);
    make_data(&packet, 1, "later");
    TEST_ASSERT_EQUAL_INT(0, lab_receiver_packet(&receiver, &packet, &ack,
                                                  delivered, sizeof(delivered),
                                                  &delivered_length));
    TEST_ASSERT_EQUAL_UINT32(0, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 0, "first");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(5, delivered_length);
    TEST_ASSERT_EQUAL_MEMORY("first", delivered, 5);

    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 1, "");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(2, ack.seq);
    TEST_ASSERT_EQUAL_UINT32(0, delivered_length);

    make_data(&packet, 2, "too large");
    lab_receiver_packet(&receiver, &packet, &ack, delivered, 1,
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(2, ack.seq);
}

/*
 * FIN is accepted only at the expected sequence.  Repeating it after
 * completion must produce the same final ACK and no additional delivery.
 * Null arguments are also checked because this function is public API.
 */
void test_receiver_fin_and_repeated_fin(void)
{
    lab_receiver_t receiver;
    lab_packet_t packet, ack;
    uint8_t delivered[1];
    size_t delivered_length;

    lab_receiver_init(&receiver);
    memset(&packet, 0, sizeof(packet));
    packet.type = LAB_FIN;
    packet.seq = 0;
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    lab_receiver_packet(&receiver, &packet, &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(1, ack.seq);
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(NULL, &packet, &ack, delivered,
                                                   sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, NULL, &ack,
                                                   delivered, sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, &packet, NULL,
                                                   delivered, sizeof(delivered),
                                                   &delivered_length));
    TEST_ASSERT_EQUAL_INT(-1, lab_receiver_packet(&receiver, &packet, &ack,
                                                   delivered, sizeof(delivered),
                                                   NULL));
}

/*
 * Cover initial window filling, timer-not-yet-expired behavior, whole-window
 * retransmission, cumulative ACK sliding, duplicate/future ACK handling,
 * default normalization, zero-capacity output, and ten-timeout give-up.
 */
void test_sender_window_ack_timeout_and_giveup(void)
{
    lab_packet_t packets[5], *out[LAB_MAX_PAYLOAD];
    lab_sender_t sender;
    size_t count;
    unsigned i;

    for (i = 0; i < 4; ++i) make_data(&packets[i], i, "x");
    memset(&packets[4], 0, sizeof(packets[4]));
    packets[4].type = LAB_FIN;
    packets[4].seq = 4;
    lab_sender_init(&sender, packets, 5, 3, 10);
    count = lab_sender_start(&sender, 0, out, 64);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT32(3, sender.next);
    count = lab_sender_timeout(&sender, 9, out, 64);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    count = lab_sender_timeout(&sender, 10, out, 64);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_PTR(&packets[0], out[0]);
    count = lab_sender_ack(&sender, 3, 11, out, 64);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_PTR(&packets[3], out[0]);
    TEST_ASSERT_EQUAL_PTR(&packets[4], out[1]);
    TEST_ASSERT_EQUAL_UINT32(3, sender.base);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 3, 12, out, 64));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 5, 13, out, 64));
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);

    lab_sender_init(&sender, packets, 1, 1, 1);
    lab_sender_start(&sender, 0, out, 64);
    for (i = 0; i < 10; ++i) {
        count = lab_sender_timeout(&sender, (uint64_t)(i + 1), out, 64);
        if (i < 9) TEST_ASSERT_EQUAL_UINT32(1, count);
        else TEST_ASSERT_EQUAL_UINT32(0, count);
    }
    TEST_ASSERT_EQUAL_INT(-1, lab_sender_done(&sender));

    lab_sender_init(&sender, packets, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_start(&sender, 0, out, 0));
    TEST_ASSERT_FALSE(sender.timer_running);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 0, out, 0));

    lab_sender_init(&sender, packets, 1, 1, 10);
    lab_sender_start(&sender, 0, out, 0);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 2, 1, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_ack(&sender, 0, 1, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 5, out, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 10, out, 0));

    lab_sender_init(&sender, packets, 2, 2, 10);
    lab_sender_start(&sender, 0, out, 2);
    TEST_ASSERT_EQUAL_UINT32(0, lab_sender_timeout(&sender, 10, out, 0));
}

/*
 * Run a small transfer through a deterministic in-memory channel that drops
 * selected packets and ACKs.  This verifies recovery without involving the
 * relay or any operating-system I/O.
 */
void test_empty_transfer_and_lossy_state_machine_transfer(void)
{
    lab_packet_t packets[4], *sent[64], ack, *retransmit[64];
    lab_sender_t sender;
    lab_receiver_t receiver;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    char result[2048];
    size_t sent_count, delivered_length, result_length = 0;
    uint64_t now = 0;
    unsigned guard = 0;

    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = 1024;
    memset(packets[0].payload, 'A', sizeof(packets[0].payload));
    make_data(&packets[1], 1, "tail");
    packets[2].type = LAB_FIN;
    packets[2].seq = 2;
    lab_sender_init(&sender, packets, 3, 2, 5);
    lab_receiver_init(&receiver);
    sent_count = lab_sender_start(&sender, now, sent, 64);
    while (!lab_sender_done(&sender) && guard++ < 100) {
        size_t i;
        for (i = 0; i < sent_count; ++i) {
            int drop = ((guard + i) % 5 == 0);
            if (drop) continue;
            lab_receiver_packet(&receiver, sent[i], &ack, delivered,
                                sizeof(delivered), &delivered_length);
            if (delivered_length) {
                memcpy(result + result_length, delivered, delivered_length);
                result_length += delivered_length;
            }

            if ((guard + i) % 7 != 0) {
                lab_sender_ack(&sender, ack.seq, now, retransmit, 64);
            }
        }
        now += 5;
        sent_count = lab_sender_timeout(&sender, now, sent, 64);
        if (sent_count == 0 && !sender.timer_running) break;
    }
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1028, result_length);
    TEST_ASSERT_EQUAL_CHAR('A', result[0]);
    TEST_ASSERT_EQUAL_CHAR('t', result[1024]);
}

static unsigned test_random(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fffu;
}

/*
 * Apply an independently seeded channel event in each direction:
 *   [0,20)   drop the packet
 *   [20,40)  corrupt one byte/sequence bit
 *   [40,60)  duplicate the packet
 *   [60,100) deliver once
 *
 * ACKs pass through the same model, so both lost/corrupted DATA and lost/
 * corrupted ACKs must be recovered by the sender timer.
 */
static void deliver_with_channel(lab_packet_t *packet, lab_receiver_t *receiver,
                                 lab_sender_t *sender, unsigned *seed,
                                 char *result, size_t *result_length)
{
    lab_packet_t received = *packet;
    lab_packet_t ack;
    lab_packet_t *new_out[64];
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t delivered_length;
    unsigned action = test_random(seed) % 100u;
    unsigned copies = action >= 40u && action < 60u ? 2u : 1u;
    unsigned copy;

    if (action < 20u) return;
    if (action >= 20u && action < 40u) {
        if (received.length != 0) {
            received.payload[test_random(seed) % received.length] ^= 1u;
        } else {
            received.seq ^= 1u;
        }
    }
    for (copy = 0; copy < copies; ++copy) {
        lab_receiver_packet(receiver, &received, &ack, delivered,
                            sizeof(delivered), &delivered_length);
        if (delivered_length != 0) {
            memcpy(result + *result_length, delivered, delivered_length);
            *result_length += delivered_length;
        }
        action = test_random(seed) % 100u;
        if (action < 20u) continue;
        if (action >= 20u && action < 40u) ack.seq ^= 1u;
        lab_sender_ack(sender, ack.seq, *result_length, new_out, 64);
    }
}

/*
 * A fixed seed makes this lossy bidirectional scenario reproducible.  The
 * expected byte string is compared after the receiver has accepted DATA in
 * order and the sender has completed the FIN exchange.
 */
void test_seeded_bidirectional_lossy_transfer(void)
{
    lab_packet_t packets[4], *out[64];
    lab_sender_t sender;
    lab_receiver_t receiver;
    char expected[2048], result[2048];
    size_t sent_count, result_length = 0;
    uint64_t now = 0;
    unsigned seed = 7;
    unsigned guard = 0;

    memset(expected, 'A', 1024);
    memcpy(expected + 1024, "reliable-transfer", 17);
    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = 1024;
    memset(packets[0].payload, 'A', 1024);
    packets[1].type = LAB_DATA;
    packets[1].seq = 1;
    packets[1].length = 17;
    memcpy(packets[1].payload, "reliable-transfer", 17);
    packets[2].type = LAB_FIN;
    packets[2].seq = 2;

    lab_sender_init(&sender, packets, 3, 2, 10);
    lab_receiver_init(&receiver);
    sent_count = lab_sender_start(&sender, now, out, 64);
    while (!lab_sender_done(&sender) && guard++ < 500) {
        size_t i;
        for (i = 0; i < sent_count; ++i) {
            deliver_with_channel(out[i], &receiver, &sender, &seed,
                                 result, &result_length);
        }
        now += 10;
        sent_count = lab_sender_timeout(&sender, now, out, 64);
    }
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_EQUAL_UINT32(1041, result_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, result_length);
}

/*
 * Verify the two file-boundary cases called out by the assignment: an empty
 * file is represented by FIN sequence zero, while a file exactly 1024 bytes
 * long is one full DATA packet followed by FIN.
 */
void test_empty_and_exact_multiple_transfers(void)
{
    lab_packet_t packets[2], *out[4], ack;
    lab_sender_t sender;
    lab_receiver_t receiver;
    uint8_t delivered[LAB_MAX_PAYLOAD];
    size_t count, delivered_length;

    memset(packets, 0, sizeof(packets));
    packets[0].type = LAB_FIN;
    packets[0].seq = 0;
    lab_sender_init(&sender, packets, 1, 1, 10);
    lab_receiver_init(&receiver);
    count = lab_sender_start(&sender, 0, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    lab_receiver_packet(&receiver, out[0], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    lab_sender_ack(&sender, ack.seq, 1, out, 4);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);

    memset(&packets[0], 0, sizeof(packets[0]));
    packets[0].type = LAB_DATA;
    packets[0].seq = 0;
    packets[0].length = LAB_MAX_PAYLOAD;
    memset(packets[0].payload, 'Z', LAB_MAX_PAYLOAD);
    packets[1].type = LAB_FIN;
    packets[1].seq = 1;
    lab_sender_init(&sender, packets, 2, 2, 10);
    lab_receiver_init(&receiver);
    count = lab_sender_start(&sender, 0, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    lab_receiver_packet(&receiver, out[0], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    TEST_ASSERT_EQUAL_UINT32(LAB_MAX_PAYLOAD, delivered_length);
    lab_sender_ack(&sender, ack.seq, 1, out, 4);
    lab_receiver_packet(&receiver, out[1], &ack, delivered, sizeof(delivered),
                        &delivered_length);
    lab_sender_ack(&sender, ack.seq, 2, out, 4);
    TEST_ASSERT_TRUE(receiver.finished);
    TEST_ASSERT_TRUE(lab_sender_done(&sender) > 0);
}

int main(void)
{
    UNITY_BEGIN();
    /* Packet-format and checksum contract. */
    RUN_TEST(test_checksum_examples_and_odd_length);
    RUN_TEST(test_packet_round_trip_and_validation);
    RUN_TEST(test_packet_encode_rejects_bad_arguments);
    RUN_TEST(test_packet_decode_rejects_each_header_error);
    /* Receiver sequencing and cumulative-ACK contract. */
    RUN_TEST(test_receiver_orders_and_reacknowledges);
    RUN_TEST(test_receiver_fin_and_repeated_fin);
    /* Sender window, timer, retransmission, and file-boundary contracts. */
    RUN_TEST(test_sender_window_ack_timeout_and_giveup);
    RUN_TEST(test_empty_transfer_and_lossy_state_machine_transfer);
    RUN_TEST(test_seeded_bidirectional_lossy_transfer);
    RUN_TEST(test_empty_and_exact_multiple_transfers);
    RUN_TEST(test_main_cli_and_clean_transfer);
    RUN_TEST(test_receiver_fwrite_failure);
    return UNITY_END();
}

```

## Scripts Files
Report generated on 09/29/2026 at 23:35:25


---

## End of Report

SHA-256 Hash of the report: 8b4af1fe40b81f4fc6b2d97d290eb9b3c8879e844caa35f25e799b4085a2a9f8

Do not edit the generated report. Any changes will be reported as academic dishonesty

---
## GitHub Info
- GitHub repo name: HenrikAckler/cs425-p2
- The repository visibility is public.
- The workflow was triggered by HenrikAckler
