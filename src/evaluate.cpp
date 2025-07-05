/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include "bitboard.h"
#include "movegen.h"
#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace Stockfish {

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the side to move. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos) {
    Color c = pos.side_to_move();
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

bool Eval::use_smallnet(const Position& pos) { return std::abs(simple_eval(pos)) > 962; }

namespace {

int mobility(const Position& p, Color c)
{
    Bitboard occ      = p.pieces();
    Bitboard friendly = p.pieces(c);
    Bitboard pawns    = p.pieces(c, PAWN);
    int      count    = 0;

    Bitboard single = (c == WHITE ? shift<NORTH>(pawns) : shift<SOUTH>(pawns)) & ~occ;
    count += popcount(single);

    Bitboard dblRank = c == WHITE ? Rank3BB : Rank6BB;
    count += popcount((c == WHITE ? shift<NORTH>(single & dblRank) : shift<SOUTH>(single & dblRank)) & ~occ);

    Bitboard pawnAtt = (c == WHITE ? shift<NORTH_EAST>(pawns) | shift<NORTH_WEST>(pawns)
                                   : shift<SOUTH_EAST>(pawns) | shift<SOUTH_WEST>(pawns));
    count += popcount(pawnAtt & ~friendly);

    Bitboard pieces = p.pieces(c, KNIGHT);
    while (pieces)
        count += popcount(attacks_bb<KNIGHT>(pop_lsb(pieces)) & ~friendly);

    pieces = p.pieces(c, BISHOP);
    while (pieces)
        count += popcount(attacks_bb<BISHOP>(pop_lsb(pieces), occ) & ~friendly);

    pieces = p.pieces(c, ROOK);
    while (pieces)
        count += popcount(attacks_bb<ROOK>(pop_lsb(pieces), occ) & ~friendly);

    pieces = p.pieces(c, QUEEN);
    while (pieces)
        count += popcount(attacks_bb<QUEEN>(pop_lsb(pieces), occ) & ~friendly);

    Square k = p.square<KING>(c);
    count += popcount(attacks_bb<KING>(k) & ~friendly);

    return count;
}

} // namespace

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Networks&    networks,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    (void)networks;
    (void)accumulators;
    (void)caches;
    (void)optimism;

    assert(!pos.checkers());

    Color us   = pos.side_to_move();
    int mob_us = mobility(pos, us);
    int mob_op = mobility(pos, ~us);

    return Value((mob_us - mob_op) * 10);
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Networks& networks) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    Eval::NNUE::AccumulatorStack accumulators;
    auto                         caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = networks.big.evaluate(pos, accumulators, &caches->big);
    Value v                 = psqt + positional;
    v                       = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    v = evaluate(networks, pos, accumulators, *caches, VALUE_ZERO);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Stockfish
