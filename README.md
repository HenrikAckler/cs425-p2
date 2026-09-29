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
