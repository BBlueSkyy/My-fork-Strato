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

    template <typename Order>
    std::array<int, 4> Assign(const Order &order, const std::array<bool, 4> &connected) {
        std::array<int, 4> slots{-1, -1, -1, -1};
        std::array<bool, 4> used{};
        constexpr std::array<std::size_t, 4> ids{0, 1, 2, 3};
        order.Assign(ids, [&](std::size_t pad, std::size_t source) {
            if (!connected[source] || used[source])
                return false;
            used[source] = true;
            slots[pad] = static_cast<int>(source);
            return true;
        });
        return slots;
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

    void TestUnavailableSourceFallsBack() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 3);
        order.Remember(1, 1);
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{0, 1, -1, -1},
                "unavailable preferred controller falls back without taking a reserved one");
    }

    void TestTemporaryStyleRestrictionPreservesSwap() {
        NpadAssignmentOrder<4, 4> order;
        order.Remember(0, 1);
        order.Remember(1, 0);
        // Neither controller is eligible during this update; both logical Npads disconnect.
        Require(Assign(order, {false, false, false, false}) == std::array<int, 4>{-1, -1, -1, -1},
                "style restriction disconnects both Npads");
        order.Observe(0, -1);
        order.Observe(1, -1);
        Require(Assign(order, {true, true, false, false}) == std::array<int, 4>{1, 0, -1, -1},
                "restoring supported styles must restore the swapped player assignment");
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
    TestUnavailableSourceFallsBack();
    TestTemporaryStyleRestrictionPreservesSwap();
    TestResetRestoresDefaultOrder();
    std::cout << "Npad assignment tests passed\n";
}
