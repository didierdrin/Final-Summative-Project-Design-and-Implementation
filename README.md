# Blockchain-Based Library Book Lending Tracker

A command-line library lending ledger written in C. Borrows, returns, copy inventory, reservations, fines, and fraud review are recorded as signed, proof-of-work blocks. Returns earn token rewards that only reach member balances after mining confirms them. The project is for the ALU Software Engineering: Low-Level Specialization, Final Summative.

---

## 1. Features

### Implemented
- Book, member, and copy registries loaded from `books.txt`, `members.txt`, and `copies.txt`
- Borrow and return by copy, with state tracking (AVAILABLE, BORROWED, RESERVED, LOST)
- Reservation queue per title, with the copy offered to the next member on return
- Renewal of a loan, blocked when another member has a reservation
- Overdue scanning, with a fine per day overdue, and fine settlement
- Token rewards: 10 coins for an on-time return, 5 for a late return
- Reward transactions held in a pending pool until mined
- Proof-of-work mining (solo) with configurable difficulty (1 to 4) and a hash attempt count
- ECDSA P-256 signatures on every record, verified during validation
- SHA-256 block hashing and hash-pointer linkage between blocks
- Merkle root stored in each block header and checked during validation
- Chain validation covering linkage, timestamps, signatures, Merkle roots, hashes, and proof-of-work
- Tamper-detection demonstration on an in-memory copy of the chain
- Balances rebuilt from the saved chain on startup
- Two balance models, UTXO and account-based, selectable in the menu
- Fraud review queue for suspicious requests, with approve and reject actions
- Binary persistence of the chain to `chain.dat`

### Not yet implemented
- Pool mining (shared rewards, 2% pool fee, reward-share table)
- Cloud mining (rental rounds, per-round fee, earnings summary, warning when fees exceed rewards)
- UTXO transfers with inputs, outputs, a fixed fee, change outputs, and double-spend checks
- Account-to-account transfers with insufficient-balance rejection and outgoing nonce checks
- Per-member ECDSA key pairs (the current build uses one node key)
- Nonce-based replay protection on requests
- Automatic fraud heuristics (concurrent borrow limit, 24-hour re-borrow, duplicate request ID, condition downgrade)

---

## 2. Dependencies

- GCC or Clang with C11 support
- OpenSSL development libraries (`libssl`, `libcrypto`)
- A POSIX shell, or MinGW on Windows

Install on Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential libssl-dev
```

Install on Fedora or RHEL:

```bash
sudo dnf install gcc openssl-devel
```

Install on macOS with Homebrew:

```bash
brew install openssl
export CFLAGS="-I$(brew --prefix openssl)/include"
export LDFLAGS="-L$(brew --prefix openssl)/lib"
```

---

## 3. Build

From the project root:

```bash
gcc -std=c11 -Wall -Wextra -Iinclude src/*.c -o library_tracker -lssl -lcrypto
```

On macOS, add `$CFLAGS $LDFLAGS` to the command.

---

## 4. Run

```bash
./library_tracker
```

On Windows (MinGW), run `library_tracker.exe`.

On first run the program:
- generates an ECDSA key pair in `keys/` if none exists;
- creates a genesis block in `chain.dat` if no chain file exists;
- validates the saved chain before showing the menu.

To start from a clean state, delete the chain file and the keys:

```bash
rm -f chain.dat
rm -rf keys
./library_tracker
```

Note: the chain file format changed (magic `LBCT2`). Delete any `chain.dat` created by an earlier build before running.

---

## 5. Data Files

All data files are read from the project root.

| File | Format | Example |
|---|---|---|
| `books.txt` | `book_id,title,author` | `B001,Introduction to Algorithms,Cormen` |
| `members.txt` | `member_id,full_name,course_code` | `M001,Jane Doe,SE101` |
| `copies.txt` | Check the format used in your file | One line per physical copy |

Lines starting with `#` and blank lines are ignored. The program exits with an error if a registry file is missing or empty.

---

## 6. Switching Between Transaction Models

Choose the model from the in-app menu:

- **UTXO model**: balances are the sum of unspent outputs per member.
- **Account-based model**: each member has a balance and a nonce.

The chosen model applies to all token balances for the rest of the session. Both views are shown by the balance command. Note that the UTXO and account transfer commands are not yet implemented, so the switch currently changes how rewards are recorded and displayed.

---

## 7. Setting Mining Difficulty

Set difficulty from the in-app menu, with a value from 1 to 4:

- difficulty 1: hash must start with `0`
- difficulty 2: hash must start with `00` (default)
- difficulty 4: hash must start with `0000`

Higher values need more hash attempts. Each mined block prints its attempt count.

---

## 8. Testing Mining

1. Start the program and borrow a book (choose the borrow option and enter a book ID and member ID).
2. Return the book, answering "y" for on time or "n" for late. The reward is added to the pending pool.
3. View the pending pool.
4. Set difficulty to 2 (or any value from 1 to 4).
5. Run mining on the pending pool. The output shows each block's attempt count and the updated balances.
6. Validate the chain. It should report valid.
7. Restart the program. Balances should be unchanged.

Solo mining is the only mining method implemented. Pool and cloud mining are listed under Not yet implemented.

---

## 9. Test Scenarios

| Scenario | Steps | Expected result |
|---|---|---|
| On-time return | Borrow, return with "y", mine | Member receives 10 tokens after mining |
| Late return | Borrow, return with "n", mine | Member receives 5 tokens after mining |
| Reward not yet mined | Return, check balance before mining | Balance unchanged |
| Invalid book or member | Borrow with an unknown ID | Error, no record created |
| Copy already on loan | Borrow a copy twice | Second borrow rejected or routed to reservation |
| Reservation offer | Reserve a title with no copies free, return one copy | Copy offered to the reserving member |
| Tamper detection | Run the tamper demo, then validate | Tampered copy reported invalid; live chain unchanged |
| Restart persistence | Mine, exit, restart | Chain and balances restored |

---

## 10. Project Structure

```text
.
├── README.md
├── report.md
├── voiceover_script.md
├── include/
│   ├── blockchain.h
│   ├── crypto_utils.h
│   └── registry.h
├── src/
│   ├── blockchain.c
│   ├── crypto_utils.c
│   ├── main.c
│   └── registry.c
├── books.txt
├── members.txt
├── copies.txt
├── keys/            (generated on first run)
└── chain.dat        (generated on first run)
```

---

## 11. Limitations

The following are known gaps. The technical report describes each one in more detail.

- Pool and cloud mining are not implemented.
- The UTXO model does not yet handle transfers, fees, change, or double spends.
- Account transfers and nonce checks are not yet enforced.
- Keys are per node, not per member.
- Fraud heuristics are not triggered automatically; flagged requests come from the review queue only.