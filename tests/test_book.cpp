// Rule-by-rule tests, run against the reference and every fast book.
#include <gtest/gtest.h>

#include <vector>

#include "lob/book.hpp"
#include "lob/book_list.hpp"
#include "lob/reference_book.hpp"

using namespace lob;

template <class B>
class BookTest : public ::testing::Test {
protected:
    B book{BookConfig{1000, 8}};
    std::vector<OutEvent> out;

    // Each helper returns only the events produced by that call.
    std::vector<OutEvent> add(OrderId id, Side s, Price px, Qty q, OrderType t = OrderType::Limit, OwnerId owner = 0) {
        InEvent e;
        e.kind = InKind::New; e.id = id; e.side = s; e.price = px; e.qty = q; e.type = t; e.owner = owner;
        return run(e);
    }
    std::vector<OutEvent> cancel(OrderId id) {
        InEvent e;
        e.kind = InKind::Cancel; e.id = id;
        return run(e);
    }
    std::vector<OutEvent> modify(OrderId id, Price px, Qty q) {
        InEvent e;
        e.kind = InKind::Modify; e.id = id; e.price = px; e.qty = q;
        return run(e);
    }
    std::vector<OutEvent> run(const InEvent& e) {
        std::vector<OutEvent> v;
        book.apply(e, [&](const OutEvent& o) { v.push_back(o); });
        return v;
    }
};

static constexpr Side B = Side::Buy, S = Side::Sell;
static OutEvent trade(OrderId taker, Side side, OrderId maker, Price px, Qty q) {
    return make_out(OutKind::Trade, Reason::None, taker, side, px, q, maker);
}
static OutEvent ev(OutKind k, Reason r, OrderId id, Side s, Price px, Qty q) { return make_out(k, r, id, s, px, q); }
using V = std::vector<OutEvent>;

using Books = ::testing::Types<ReferenceBook, BookV1, BookV2, BookV3, BookV3Bitmap>;
TYPED_TEST_SUITE(BookTest, Books);

TYPED_TEST(BookTest, EmptyBook) {
    EXPECT_EQ(this->book.best_bid(), 0);
    EXPECT_EQ(this->book.best_ask(), 0);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(BookTest, LimitOrderRestsAndSetsBest) {
    EXPECT_EQ(this->add(1, B, 100, 10), (V{ev(OutKind::Accepted, Reason::None, 1, B, 100, 10)}));
    this->add(2, B, 99, 5);
    this->add(3, S, 105, 7);
    this->add(4, S, 103, 2);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 103);
    EXPECT_EQ(this->book.level_qty(B, 100), 10u);
    EXPECT_EQ(this->book.order_count(), 4u);
}

TYPED_TEST(BookTest, PriceThenTimePriority) {
    this->add(1, S, 101, 5);
    this->add(2, S, 100, 5);  // better price, arrives later
    this->add(3, S, 100, 5);  // same price as 2, behind it
    EXPECT_EQ(this->add(9, B, 101, 12),
              (V{trade(9, B, 2, 100, 5), trade(9, B, 3, 100, 5), trade(9, B, 1, 101, 2)}));
    EXPECT_EQ(this->book.open_qty(1), 3u);
    EXPECT_EQ(this->book.best_ask(), 101);
}

TYPED_TEST(BookTest, TradesAtMakerPriceAndRestsRemainder) {
    this->add(1, B, 100, 4);
    EXPECT_EQ(this->add(2, S, 95, 10),
              (V{trade(2, S, 1, 100, 4), ev(OutKind::Accepted, Reason::None, 2, S, 95, 6)}));
    EXPECT_EQ(this->book.best_bid(), 0);
    EXPECT_EQ(this->book.best_ask(), 95);
}

TYPED_TEST(BookTest, MarketOrderSweepsThenCancelsRemainder) {
    this->add(1, S, 100, 3);
    this->add(2, S, 500, 3);
    EXPECT_EQ(this->add(3, B, 0, 10, OrderType::Market),
              (V{trade(3, B, 1, 100, 3), trade(3, B, 2, 500, 3), ev(OutKind::Cancelled, Reason::Unfilled, 3, B, 0, 4)}));
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_EQ(this->add(4, S, 0, 1, OrderType::Market), (V{ev(OutKind::Cancelled, Reason::Unfilled, 4, S, 0, 1)}));
}

TYPED_TEST(BookTest, IocNeverRests) {
    this->add(1, S, 100, 3);
    this->add(2, S, 102, 3);
    EXPECT_EQ(this->add(3, B, 101, 5, OrderType::IOC),
              (V{trade(3, B, 1, 100, 3), ev(OutKind::Cancelled, Reason::Unfilled, 3, B, 101, 2)}));
    EXPECT_EQ(this->book.best_bid(), 0);
    EXPECT_EQ(this->book.best_ask(), 102);
}

TYPED_TEST(BookTest, FokIsAllOrNothing) {
    this->add(1, S, 100, 3);
    this->add(2, S, 101, 3);
    EXPECT_EQ(this->add(3, B, 101, 7, OrderType::FOK), (V{ev(OutKind::Cancelled, Reason::FokUnfillable, 3, B, 101, 7)}));
    EXPECT_EQ(this->add(4, B, 100, 4, OrderType::FOK), (V{ev(OutKind::Cancelled, Reason::FokUnfillable, 4, B, 100, 4)}));
    EXPECT_EQ(this->book.order_count(), 2u);  // untouched
    EXPECT_EQ(this->add(5, B, 101, 6, OrderType::FOK), (V{trade(5, B, 1, 100, 3), trade(5, B, 2, 101, 3)}));
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(BookTest, PostOnlyRejectsIfItWouldCross) {
    this->add(1, S, 100, 3);
    EXPECT_EQ(this->add(2, B, 100, 1, OrderType::PostOnly), (V{ev(OutKind::Cancelled, Reason::PostOnlyCross, 2, B, 100, 1)}));
    EXPECT_EQ(this->add(3, B, 99, 1, OrderType::PostOnly), (V{ev(OutKind::Accepted, Reason::None, 3, B, 99, 1)}));
    EXPECT_EQ(this->book.open_qty(1), 3u);
}

TYPED_TEST(BookTest, CancelRemovesAndUnknownIsRejected) {
    this->add(1, B, 100, 3);
    this->add(2, B, 100, 4);
    EXPECT_EQ(this->cancel(1), (V{ev(OutKind::Cancelled, Reason::UserCancel, 1, B, 100, 3)}));
    EXPECT_EQ(this->cancel(1), (V{ev(OutKind::Rejected, Reason::UnknownOrder, 1, B, 0, 0)}));
    EXPECT_EQ(this->book.level_qty(B, 100), 4u);
    this->add(3, S, 100, 4);  // fills order 2 completely
    EXPECT_EQ(this->cancel(2), (V{ev(OutKind::Rejected, Reason::UnknownOrder, 2, B, 0, 0)}));
    EXPECT_EQ(this->book.best_bid(), 0);
}

TYPED_TEST(BookTest, BestPriceSkipsGapsAfterLevelEmpties) {
    this->add(1, B, 900, 1);
    this->add(2, B, 10, 1);
    this->add(3, S, 950, 1);
    this->add(4, S, 999, 1);
    this->cancel(1);
    EXPECT_EQ(this->book.best_bid(), 10);
    this->cancel(3);
    EXPECT_EQ(this->book.best_ask(), 999);
    this->cancel(2);
    this->cancel(4);
    EXPECT_EQ(this->book.best_bid(), 0);
    EXPECT_EQ(this->book.best_ask(), 0);
    this->add(5, B, 1, 1);
    this->add(6, S, 1000, 1);
    EXPECT_EQ(this->book.best_bid(), 1);
    EXPECT_EQ(this->book.best_ask(), 1000);
}

TYPED_TEST(BookTest, ModifyDownKeepsPriority) {
    this->add(1, S, 100, 5);
    this->add(2, S, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 2), (V{ev(OutKind::Modified, Reason::None, 1, S, 100, 2)}));
    EXPECT_EQ(this->book.level_qty(S, 100), 7u);
    EXPECT_EQ(this->add(3, B, 100, 3), (V{trade(3, B, 1, 100, 2), trade(3, B, 2, 100, 1)}));
}

TYPED_TEST(BookTest, ModifyUpOrRepriceLosesPriority) {
    this->add(1, S, 100, 5);
    this->add(2, S, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 6), (V{ev(OutKind::Cancelled, Reason::Replaced, 1, S, 100, 5),
                                          ev(OutKind::Accepted, Reason::None, 1, S, 100, 6)}));
    EXPECT_EQ(this->add(3, B, 100, 5), (V{trade(3, B, 2, 100, 5)}));  // order 2 is now first
    this->add(4, B, 90, 5);
    // Repricing through the book trades immediately, as a normal limit order would.
    EXPECT_EQ(this->modify(1, 90, 8), (V{ev(OutKind::Cancelled, Reason::Replaced, 1, S, 100, 6),
                                         trade(1, S, 4, 90, 5), ev(OutKind::Accepted, Reason::None, 1, S, 90, 3)}));
}

TYPED_TEST(BookTest, ModifyRejections) {
    this->add(1, B, 100, 5);
    EXPECT_EQ(this->modify(7, 100, 5), (V{ev(OutKind::Rejected, Reason::UnknownOrder, 7, B, 0, 0)}));
    EXPECT_EQ(this->modify(1, 100, 0), (V{ev(OutKind::Rejected, Reason::BadQty, 1, B, 100, 0)}));
    EXPECT_EQ(this->modify(1, 1001, 5), (V{ev(OutKind::Rejected, Reason::BadPrice, 1, B, 1001, 5)}));
    EXPECT_EQ(this->book.open_qty(1), 5u);
}

TYPED_TEST(BookTest, SelfTradePreventionCancelsIncomingRemainder) {
    this->add(1, S, 100, 3, OrderType::Limit, 7);
    this->add(2, S, 101, 3, OrderType::Limit, 8);
    this->add(3, S, 101, 3, OrderType::Limit, 7);
    // Owner 8 buys: trades with owner 7 at 100, then meets its own order at 101 and stops.
    EXPECT_EQ(this->add(4, B, 105, 9, OrderType::Limit, 8),
              (V{trade(4, B, 1, 100, 3), ev(OutKind::Cancelled, Reason::SelfTrade, 4, B, 105, 6)}));
    EXPECT_EQ(this->book.open_qty(2), 3u);  // resting order untouched
    EXPECT_EQ(this->book.best_bid(), 0);    // the incoming remainder did not rest
    // FOK is unfillable if its own order stands in the way.
    EXPECT_EQ(this->add(5, B, 105, 4, OrderType::FOK, 7), (V{ev(OutKind::Cancelled, Reason::FokUnfillable, 5, B, 105, 4)}));
    // Owner 0 opts out of the check.
    this->add(6, S, 99, 1, OrderType::Limit, 0);
    EXPECT_EQ(this->add(7, B, 99, 1, OrderType::Limit, 0), (V{trade(7, B, 6, 99, 1)}));
}

TYPED_TEST(BookTest, InputRejections) {
    EXPECT_EQ(this->add(1, B, 100, 0), (V{ev(OutKind::Rejected, Reason::BadQty, 1, B, 100, 0)}));
    EXPECT_EQ(this->add(1, B, 0, 5), (V{ev(OutKind::Rejected, Reason::BadPrice, 1, B, 0, 5)}));
    EXPECT_EQ(this->add(1, B, 1001, 5), (V{ev(OutKind::Rejected, Reason::BadPrice, 1, B, 1001, 5)}));
    EXPECT_EQ(this->add(1, B, -3, 5, OrderType::IOC), (V{ev(OutKind::Rejected, Reason::BadPrice, 1, B, -3, 5)}));
    this->add(1, B, 100, 5);
    EXPECT_EQ(this->add(1, S, 200, 5), (V{ev(OutKind::Rejected, Reason::DuplicateId, 1, S, 200, 5)}));
    this->cancel(1);
    EXPECT_EQ(this->add(1, S, 200, 5), (V{ev(OutKind::Accepted, Reason::None, 1, S, 200, 5)}));  // id reusable once dead
}

TYPED_TEST(BookTest, BookFullRejectsOnlyOrdersThatCouldRest) {
    for (OrderId i = 1; i <= 8; ++i) this->add(i, S, 100 + static_cast<Price>(i), 1);
    EXPECT_EQ(this->add(9, B, 50, 1), (V{ev(OutKind::Rejected, Reason::BookFull, 9, B, 50, 1)}));
    EXPECT_EQ(this->add(10, B, 101, 1, OrderType::IOC), (V{trade(10, B, 1, 101, 1)}));
    EXPECT_EQ(this->add(11, B, 50, 1), (V{ev(OutKind::Accepted, Reason::None, 11, B, 50, 1)}));
    // A replace frees its own slot first, so it always fits.
    EXPECT_EQ(this->modify(11, 51, 2).size(), 2u);
    EXPECT_EQ(this->book.order_count(), 8u);
}
