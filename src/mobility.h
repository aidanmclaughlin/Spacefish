/*
  Spacefish mobility utilities
  Fast mobility calculation, lightweight cache, and simple heuristics
*/

#ifndef SPACEFISH_MOBILITY_H_INCLUDED
#define SPACEFISH_MOBILITY_H_INCLUDED

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "types.h"
#include "position.h"
#include "bitboard.h"
#include "movegen.h"

namespace Stockfish {

// Mobility information stored in cache
struct MobilityInfo {
    int ourMobility;
    int theirMobility;
    int differential;

    MobilityInfo() = default;
    MobilityInfo(int our, int their) : ourMobility(our), theirMobility(their), differential(our - their) {}
};

// Fast mobility cache using Zobrist keys (64k direct-mapped)
class MobilityCache {
   private:
    static constexpr size_t CacheSize = 1u << 16; // 65536 entries

    struct Entry {
        Key         key{};
        MobilityInfo info{};
        uint8_t     generation{};
    };

    Entry   table[CacheSize];
    uint8_t currentGeneration = 1;

   public:
    MobilityCache() { std::memset(table, 0, sizeof(table)); }

    void clear() {
        // Advance generation; on wrap, clear full table
        if (++currentGeneration == 0) {
            std::memset(table, 0, sizeof(table));
            currentGeneration = 1;
        }
    }

    MobilityInfo* probe(Key key) {
        Entry& e = table[key & (CacheSize - 1)];
        if (e.key == key && e.generation == currentGeneration)
            return &e.info;
        return nullptr;
    }

    void store(Key key, const MobilityInfo& info) {
        Entry& e  = table[key & (CacheSize - 1)];
        e.key     = key;
        e.info    = info;
        e.generation = currentGeneration;
    }
};

// Simple bounds helper tuned for mobility-as-eval
struct MobilityBounds {
    static constexpr int MaxSingleMoveGain = 27;  // optimistic queen centralization
    static constexpr int MaxSingleMoveLoss = 27;

    bool canReachBeta(Value currentEval, Value beta) const {
        return currentEval + MaxSingleMoveGain * 4 >= beta; // 4 plies optimism window
    }

    bool canMaintainAlpha(Value currentEval, Value alpha) const {
        return currentEval - MaxSingleMoveLoss * 4 > alpha;
    }
};

// Precomputed helpers
struct MobilityTables {
    static constexpr int KnightMobility[64] = {
        2,3,4,4,4,4,3,2,
        3,4,6,6,6,6,4,3,
        4,6,8,8,8,8,6,4,
        4,6,8,8,8,8,6,4,
        4,6,8,8,8,8,6,4,
        4,6,8,8,8,8,6,4,
        3,4,6,6,6,6,4,3,
        2,3,4,4,4,4,3,2
    };

    static int slidingPieceMobility(Square s, PieceType pt) {
        int r = rank_of(s);
        int f = file_of(s);
        int centerDistance = std::max(std::abs(r - 3), std::abs(f - 3));
        if (pt == BISHOP) return 13 - centerDistance * 2;
        if (pt == ROOK)   return 14;
        return 27 - centerDistance; // queen
    }
};

// Optimized mobility calculation using bitboards (pseudo-legal reach)
inline int fast_mobility(const Position& pos, Color c) {
    Bitboard occupied = pos.pieces();
    Bitboard friendly = pos.pieces(c);
    Bitboard enemy    = pos.pieces(~c);
    int mobility = 0;

    // Pawns
    Bitboard pawns = pos.pieces(c, PAWN);
    if (c == WHITE) {
        mobility += popcount(shift<NORTH>(pawns) & ~occupied);
        Bitboard rank2Pawns = pawns & Rank2BB;
        mobility += popcount(shift<NORTH>(shift<NORTH>(rank2Pawns)) & ~occupied & Rank4BB);
        mobility += popcount((shift<NORTH_EAST>(pawns) | shift<NORTH_WEST>(pawns)) & enemy);
    } else {
        mobility += popcount(shift<SOUTH>(pawns) & ~occupied);
        Bitboard rank7Pawns = pawns & Rank7BB;
        mobility += popcount(shift<SOUTH>(shift<SOUTH>(rank7Pawns)) & ~occupied & Rank5BB);
        mobility += popcount((shift<SOUTH_EAST>(pawns) | shift<SOUTH_WEST>(pawns)) & enemy);
    }

    // Knights
    Bitboard knights = pos.pieces(c, KNIGHT);
    while (knights) {
        Square s = pop_lsb(knights);
        mobility += popcount(attacks_bb<KNIGHT>(s) & ~friendly);
    }

    // Bishops
    Bitboard bishops = pos.pieces(c, BISHOP);
    while (bishops) {
        Square s = pop_lsb(bishops);
        mobility += popcount(attacks_bb<BISHOP>(s, occupied) & ~friendly);
    }

    // Rooks
    Bitboard rooks = pos.pieces(c, ROOK);
    while (rooks) {
        Square s = pop_lsb(rooks);
        mobility += popcount(attacks_bb<ROOK>(s, occupied) & ~friendly);
    }

    // Queens
    Bitboard queens = pos.pieces(c, QUEEN);
    while (queens) {
        Square s = pop_lsb(queens);
        mobility += popcount(attacks_bb<QUEEN>(s, occupied) & ~friendly);
    }

    // King
    Square k = pos.square<KING>(c);
    mobility += popcount(attacks_bb<KING>(k) & ~friendly);

    return mobility;
}

// Approximate mobility delta heuristic for move ordering/reductions
inline int mobility_delta(const Position& pos, Move m) {
    Square from = m.from_sq();
    Square to   = m.to_sq();

    // Center movement heuristic
    auto centerDist = [](Square s){ return std::max(std::abs(rank_of(s) - 3), std::abs(file_of(s) - 3)); };
    int delta = (centerDist(from) - centerDist(to)) * 2;

    if (pos.capture(m)) {
        PieceType captured = type_of(pos.piece_on(to));
        if (captured == KNIGHT)
            delta += MobilityTables::KnightMobility[to];
        else if (captured != PAWN)
            delta += MobilityTables::slidingPieceMobility(to, captured);
    }

    return delta;
}

// Mobility history table for quiet move ordering/extension
class MobilityHistory {
   private:
    static constexpr int HistoryMax = 16384;
    int16_t table[COLOR_NB][64][64]{}; // [color][from][to]

   public:
    void clear() { std::memset(table, 0, sizeof(table)); }
    int  get(Color c, Move m) const { return table[c][m.from_sq()][m.to_sq()]; }
    void update(Color c, Move m, int bonus) {
        int16_t& e = table[c][m.from_sq()][m.to_sq()];
        e += bonus - e * std::abs(bonus) / HistoryMax;
        e = std::clamp(e, int16_t(-HistoryMax), int16_t(HistoryMax));
    }
};

} // namespace Stockfish

#endif // SPACEFISH_MOBILITY_H_INCLUDED

