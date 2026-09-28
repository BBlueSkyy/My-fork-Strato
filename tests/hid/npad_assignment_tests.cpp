#include <array>
#include <cstdlib>
#include <iostream>

#include "skyline/input/npad_assignment.h"

using skyline::input::NpadAssignmentOrder;

namespace {
    void Require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAIL " << message << '\n';
            std::exit(1);
        }
    }

    template <typename Order, std::size_t N>
    std::array<int, 4> AssignPads(const Order &order, const std::array<bool, 4> &connected,
                                  const std::array<std::size_t, N> &ids) {
        std::array<int, 4> slots{-1, -1, -1, -1};
        std::array<bool, 4> used{};
        order.Assign(ids, [&](std::size_t pad, std::size_t source) {
            if (!connected[source] || used[source])
                return false;
            used[source] = true;
            slots[pad] = static_cast<int>(source);
            return true;
        });
        return slots;
    }

    template <typename Order>
    std::array<int, 4> Assign(const Order &order, const std::array<bool, 4> &connected) {
        constexpr std::array<std::size_t, 4> ids{0, 1, 2, 3};
        return AssignPads(order, connected, ids);
    }

    void TestSwappedPlayersRemainSwappedOnUpdate() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 2);
        order.Remember(1, 1);
        order.Remember(2, 0);
        Require(Assign(order, {true, true, true, false}) == std::array<int, 4>{2, 1, 0, -1},
                "swapped host controllers retain their player IDs on update");
    }

    void TestSwapIntoEmptyPlayerDoesNotStealOtherPlayers() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, -1);
        order.Remember(1, 1);
        order.Remember(2, 2);
        order.Remember(3, 0);
        Require(Assign(order, {true, true, true, false}) == std::array<int, 4>{-1, 1, 2, 0},
                "swap to an empty player preserves the other connected players");
    }

    void TestUnavailableSourceFallsBackWithoutForgettingPreference() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 3);
        order.Remember(1, 1);

        auto assigned{Assign(order, {true, true, false, false})};
        Require(assigned == std::array<int, 4>{0, 1, -1, -1},
                "unavailable preferred controller falls back without taking a reserved one");

        order.Observe(0, assigned[0]);
        Require(Assign(order, {true, true, false, true}) == std::array<int, 4>{3, 1, 0, 2},
                "temporary fallback must not replace the remembered preferred source");
    }

    void TestTemporaryStyleRestrictionPreservesSwap() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 1);
        order.Remember(1, 0);
        Require(Assign(order, {false, false, false, false}) == std::array<int, 4>{-1, -1, -1, -1},
                "style restriction disconnects both Npads");
        order.Observe(0, -1);
        order.Observe(1, -1);
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{1, 0, -1, -1},
                "restoring supported styles must restore the swapped player assignment");
    }

    void TestTemporarySupportedIdRemovalPreservesEmptySwapTarget() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, -1);
        order.Remember(1, 0);

        constexpr std::array<std::size_t, 1> onlyPlayer1{0};
        const auto restricted{AssignPads(order, {true, false, false, false}, onlyPlayer1)};
        Require(restricted == std::array<int, 4>{-1, -1, -1, -1},
                "temporarily removing the source owner must not fill an explicitly empty player");

        order.Observe(0, restricted[0]);
        Require(Assign(order, {true, false, false, false}) == std::array<int, 4>{-1, 0, -1, -1},
                "restoring the removed Npad must restore the original swap into the empty player");
    }

    void TestAbsentPlayerKeepsItsSourceReserved() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 1);
        order.Remember(1, 0);

        constexpr std::array<std::size_t, 1> onlyPlayer1{0};
        const auto restricted{AssignPads(order, {true, false, true, false}, onlyPlayer1)};
        Require(restricted == std::array<int, 4>{2, -1, -1, -1},
                "fallback must not take a source reserved by a temporarily unsupported Npad");

        order.Observe(0, restricted[0]);
        Require(Assign(order, {true, true, true, false}) == std::array<int, 4>{1, 0, 2, -1},
                "restoring supported IDs must restore the original reserved assignments");
    }

    void TestRememberMovesSourceOwnership() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 0);
        order.Remember(1, 0);
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{1, 0, -1, -1},
                "one controller source must not remain preferred by two Npads");
    }

    void TestResetRestoresDefaultOrder() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 1);
        order.Remember(1, 0);
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{1, 0, -1, -1},
                "swapped order is active before reset");
        order.Reset();
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{0, 1, -1, -1},
                "reset restores the normal player order");
    }
}

int main() {
    TestSwappedPlayersRemainSwappedOnUpdate();
    TestSwapIntoEmptyPlayerDoesNotStealOtherPlayers();
    TestUnavailableSourceFallsBackWithoutForgettingPreference();
    TestTemporaryStyleRestrictionPreservesSwap();
    TestTemporarySupportedIdRemovalPreservesEmptySwapTarget();
    TestAbsentPlayerKeepsItsSourceReserved();
    TestRememberMovesSourceOwnership();
    TestResetRestoresDefaultOrder();
    std::cout << "Npad assignment tests passed\n";
}
