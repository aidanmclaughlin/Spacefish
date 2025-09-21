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
#include <iomanip>
#include <sstream>

#include "position.h"
#include "uci.h"

namespace Stockfish {

namespace {

struct MobilityMetrics {
    int  myMobility;
    int  oppMobility;
    bool myInCheck;
    bool oppInCheck;
};

constexpr int   MobilityWeight     = 32;
constexpr int   MobilityNormalizer = 64;
constexpr Value MobilityMateScore  = VALUE_MATE_IN_MAX_PLY - 1;
constexpr Value MobilityMatedScore = VALUE_MATED_IN_MAX_PLY + 1;

MobilityMetrics mobility_metrics(const Position& pos) {
    const Color us = pos.side_to_move();

    MobilityMetrics metrics{};
    metrics.myMobility  = pos.mobility(us);
    metrics.oppMobility = pos.mobility(~us);
    metrics.myInCheck   = pos.checkers();
    metrics.oppInCheck  =
      pos.attackers_to(pos.square<KING>(~us)) & pos.pieces(us) ? true : false;

    return metrics;
}

Value mobility_score(const MobilityMetrics& metrics) {
    if (metrics.myMobility == 0)
        return metrics.myInCheck ? MobilityMatedScore : VALUE_DRAW;

    if (metrics.oppMobility == 0)
        return metrics.oppInCheck ? MobilityMateScore : VALUE_DRAW;

    const int diff  = metrics.myMobility - metrics.oppMobility;
    const int total = metrics.myMobility + metrics.oppMobility + MobilityNormalizer;

    const long long scaled = static_cast<long long>(diff) * MobilityWeight * MobilityNormalizer;
    Value           score  = static_cast<Value>(scaled / total);

    return std::clamp(score, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

}  // namespace

Value Eval::evaluate(const Position& pos) {
    const MobilityMetrics metrics = mobility_metrics(pos);
    return mobility_score(metrics);
}

std::string Eval::trace(Position& pos) {
    const MobilityMetrics metrics = mobility_metrics(pos);
    const Value           score   = mobility_score(metrics);

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    const Color stm = pos.side_to_move();
    const auto   scoreCp = 0.01 * UCIEngine::to_cp(stm == WHITE ? score : -score, pos);

    ss << "Mobility summary (STM: " << (stm == WHITE ? 'w' : 'b') << ")\n";
    ss << "  My legal moves: " << metrics.myMobility;
    if (metrics.myInCheck)
        ss << " (in check)";
    ss << "\n";

    ss << "  Opp legal moves: " << metrics.oppMobility;
    if (metrics.oppInCheck)
        ss << " (in check)";
    ss << "\n";

    ss << "  Score: " << scoreCp << " pawns";

    return ss.str();
}

}  // namespace Stockfish
