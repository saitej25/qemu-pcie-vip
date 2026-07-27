#include "mini_ics/transaction.hpp"
#include "test_harness.hpp"

#include <thread>

using namespace mini_ics;
using namespace mini_ics::test;

MINI_ICS_TEST(TransactionIdsAreUniqueAndIncreasing) {
    TransactionIdGenerator gen(100);
    std::uint64_t a = gen.Next();
    std::uint64_t b = gen.Next();
    std::uint64_t c = gen.Next();
    MINI_ICS_CHECK_EQ(a, 100ull);
    MINI_ICS_CHECK_EQ(b, 101ull);
    MINI_ICS_CHECK_EQ(c, 102ull);
}

MINI_ICS_TEST(PendingRequestTableMatchesCorrectTransactionId) {
    PendingRequestTable table;
    table.RegisterPending(1);
    table.RegisterPending(2);

    Message msg_for_2;
    msg_for_2.header.txn_id = 2;
    msg_for_2.payload = {0xAA};
    MINI_ICS_CHECK(table.Complete(2, msg_for_2));

    auto result = table.WaitFor(2, 500);
    MINI_ICS_CHECK(result.has_value());
    MINI_ICS_CHECK_EQ(result->header.txn_id, 2ull);
    MINI_ICS_CHECK_EQ(result->payload[0], 0xAA);

    // Transaction 1 was never completed and must still time out cleanly,
    // proving responses aren't cross-matched to the wrong pending entry.
    auto result1 = table.WaitFor(1, 100);
    MINI_ICS_CHECK(!result1.has_value());
}

MINI_ICS_TEST(PendingRequestTableHandlesOutOfOrderResponses) {
    PendingRequestTable table;
    table.RegisterPending(10);
    table.RegisterPending(11);

    // Complete the second-registered transaction first.
    Message msg11;
    msg11.header.txn_id = 11;
    MINI_ICS_CHECK(table.Complete(11, msg11));

    Message msg10;
    msg10.header.txn_id = 10;
    MINI_ICS_CHECK(table.Complete(10, msg10));

    auto r11 = table.WaitFor(11, 500);
    auto r10 = table.WaitFor(10, 500);
    MINI_ICS_CHECK(r11.has_value());
    MINI_ICS_CHECK(r10.has_value());
    MINI_ICS_CHECK_EQ(r11->header.txn_id, 11ull);
    MINI_ICS_CHECK_EQ(r10->header.txn_id, 10ull);
}

MINI_ICS_TEST(PendingRequestTableTimesOutWithoutResponse) {
    PendingRequestTable table;
    table.RegisterPending(5);
    auto result = table.WaitFor(5, 100);
    MINI_ICS_CHECK(!result.has_value());
}

MINI_ICS_TEST(CompleteRejectsUnknownTransactionId) {
    PendingRequestTable table;
    Message msg;
    msg.header.txn_id = 999;
    MINI_ICS_CHECK(!table.Complete(999, msg));
}

MINI_ICS_TEST(BlockingQueuePushPopWorks) {
    BlockingQueue<int> q;
    q.Push(1);
    q.Push(2);
    auto a = q.Pop(100);
    auto b = q.Pop(100);
    MINI_ICS_CHECK(a.has_value());
    MINI_ICS_CHECK(b.has_value());
    MINI_ICS_CHECK_EQ(*a, 1);
    MINI_ICS_CHECK_EQ(*b, 2);
}

MINI_ICS_TEST(BlockingQueuePopTimesOutWhenEmpty) {
    BlockingQueue<int> q;
    auto result = q.Pop(100);
    MINI_ICS_CHECK(!result.has_value());
}

MINI_ICS_TEST(BlockingQueueUnblocksOnShutdown) {
    BlockingQueue<int> q;
    std::thread t([&] { q.Shutdown(); });
    auto result = q.Pop(5000);  // must return promptly via Shutdown(), not the 5s timeout
    t.join();
    MINI_ICS_CHECK(!result.has_value());
}

int main() { return RunAll("TransactionTest"); }
