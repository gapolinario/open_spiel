// Copyright 2019 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "open_spiel/games/agnes_sorel/agnes_sorel.h"

#include "open_spiel/abseil-cpp/absl/types/optional.h"
#include "open_spiel/spiel.h"
#include "open_spiel/spiel_utils.h"

#include <iostream> // TODO: remove

namespace open_spiel::agnes_sorel {

namespace {
const GameType kGameType{/*short_name=*/"agnes_sorel",
                         /*long_name=*/"Agnes Sorel Patience",
                         GameType::Dynamics::kSequential,
                         GameType::ChanceMode::kExplicitStochastic,
                         GameType::Information::kImperfectInformation,
                         GameType::Utility::kGeneralSum,
                         GameType::RewardModel::kRewards,
                         /*max_num_players=*/1,
                         /*min_num_players=*/1,
                         /*provides_information_state_string=*/true,
                         /*provides_information_state_tensor=*/false,
                         /*provides_observation_string=*/true,
                         /*provides_observation_tensor=*/true,
                         /*parameter_specification=*/
                         {{"players", GameParameter(kDefaultPlayers)},
                          {"is_colored", GameParameter(kDefaultIsColored)},
                          {"depth_limit", GameParameter(kDefaultDepthLimit)}}};

std::shared_ptr<const Game> Factory(const GameParameters& params) {
  return std::shared_ptr<const Game>(new AgnesSorelGame(params));
}

REGISTER_SPIEL_GAME(kGameType, Factory)
}  // namespace

namespace {
// ANSI color codes
inline constexpr const char* kReset = "\033[0m";
inline constexpr const char* kRed = "\033[31m";
inline constexpr const char* kBlack = "\033[37m";

// Unicode Glyphs
inline constexpr const char* kGlyphHidden = "\U0001F0A0";
inline constexpr const char* kGlyphEmpty = "\U0001F0BF";
inline constexpr const char* kGlyphSpades = "\U00002660";
inline constexpr const char* kGlyphHearts = "\U00002665";
inline constexpr const char* kGlyphClubs = "\U00002663";
inline constexpr const char* kGlyphDiamonds = "\U00002666";
inline constexpr const char* kGlyphArrow = "\U00002190";

// Constants ===================================================================
inline constexpr int kNumRanks = 13;

// Number of cards_ that can be in each pile type_
inline constexpr int kMaxSizeWaste = 23;
inline constexpr int kMaxSizeFoundation = 13;
inline constexpr int kMaxSizeTableau = 26;

// Number of sources that can be in each pile type_
inline constexpr int kMaxSourcesWaste = 8; // OBS: 2 in fact
inline constexpr int kMaxSourcesFoundation = 1;
inline constexpr int kMaxSourcesTableau = 52;

// These divide up the action ids into sections. kEnd is a single action that is
// used to end the game when no other actions are available.
inline constexpr int kEnd = 0;

// Reveal actions are ones that can be taken at chance nodes; they change a
// hidden_ card to a card of the same index_ as the action id_ (e.g. 2 would
// reveal a 2 of spades)
inline constexpr int kRevealStart = 1;
inline constexpr int kRevealEnd = 52;

// kMove actions are ones that are taken at decision nodes; they involve moving
// a card to another cards_ location_. It starts at 53 because there are 52
// reveal actions before it. See `NumDistinctActions()` in agnes_sorel.cc.
// 261-312 are moves from waste (hidden) to end of tableau
inline constexpr int kMoveStart = 53;
inline constexpr int kMoveEnd = 365;

// A single action that the player may take. This deals hidden cards
// from the waste to the tableau
// Last valid action = NumDistinctActions() - 1
inline constexpr int kDeal = 366;

// Indices for special cards_
// inline constexpr int kHiddenCard = 99;
inline constexpr int kEmptySpadeCard = -5;
inline constexpr int kEmptyHeartCard = -4;
inline constexpr int kEmptyClubCard = -3;
inline constexpr int kEmptyDiamondCard = -2;
inline constexpr int kEmptyTableauCard = -1;

// 1 empty + 13 ranks
inline constexpr int kFoundationTensorLength = 14;

// 6 hidden_ cards_ + 1 empty tableau + 52 ordinary cards_
inline constexpr int kTableauTensorLength = 59;

// 1 hidden_ card + 52 ordinary cards_
inline constexpr int kWasteTensorLength = 53;

// Constant for how many hidden_ cards_ can show up in a tableau. As hidden_
// cards_ can't be added, the max is the highest number in a tableau at the
// start of the game: 6
inline constexpr int kMaxHiddenCard = 6; // OBS: Can I change this?

// Only used in one place and just for consistency (to match kChancePlayerId&
// kTerminalPlayerId)
inline constexpr int kPlayerId = 0;

// Indicates the last index_ before the first player action (the last Reveal
// action has an ID of 52)
inline constexpr int kActionOffset = 52;

// Order of suits
const std::vector<SuitType> kSuits = {SuitType::kSpades, SuitType::kHearts,
                                      SuitType::kClubs, SuitType::kDiamonds};

// Vector with all valid ranks
const std::vector<RankType> kRanks = {RankType::kA, RankType::k2, RankType::k3,
                                      RankType::k4, RankType::k5, RankType::k6,
                                      RankType::k7, RankType::k8, RankType::k9,
                                      RankType::kT, RankType::kJ, RankType::kQ,
                                      RankType::kK};

// These correspond with their enums, not with the two vectors directly above
const std::vector<std::string> kSuitStrs = {
    "", kGlyphSpades, kGlyphHearts, kGlyphClubs, kGlyphDiamonds, ""};
const std::vector<std::string> kRankStrs = {
    "", "A", "2", "3", "4", "5", "6", "7", "8", "9", "T", "J", "Q", "K", ""};

const std::map<RankType, double> kFoundationPoints = {
    // region Maps a RankType to the reward for moving a card of that rank_ to
    // the foundation
    {RankType::kA, 100.0}, {RankType::k2, 90.0}, {RankType::k3, 80.0},
    {RankType::k4, 70.0},  {RankType::k5, 60.0}, {RankType::k6, 50.0},
    {RankType::k7, 40.0},  {RankType::k8, 30.0}, {RankType::k9, 20.0},
    {RankType::kT, 10.0},  {RankType::kJ, 10.0}, {RankType::kQ, 10.0},
    {RankType::kK, 10.0}
    // endregion
};
// OBS: Could change, 100 would correspond to the foundation_card_
// but this wouldn't be const anymore

const std::map<SuitType, PileID> kSuitToPile = {
    // region Maps a foundation suit_ to the ID of the foundation
    {SuitType::kSpades, PileID::kSpades},
    {SuitType::kHearts, PileID::kHearts},
    {SuitType::kClubs, PileID::kClubs},
    {SuitType::kDiamonds, PileID::kDiamonds}
    // endregion
};

const std::map<int, PileID> kIntToPile = {
    // region Maps an integer to a tableau pile ID (used when initializing
    // AgnesSorelState)
    {1, PileID::k1stTableau}, {2, PileID::k2ndTableau},
    {3, PileID::k3rdTableau}, {4, PileID::k4thTableau},
    {5, PileID::k5thTableau}, {6, PileID::k6thTableau},
    {7, PileID::k7thTableau}
    // endregion
};

}  // namespace

// Miscellaneous ===============================================================

std::vector<SuitType> GetSameColorSuits(const SuitType& suit) {
  /* Just returns a vector of the suits of same color. For red suits
   * (SuitType::kHearts and SuitType::kDiamonds), this returns a vector of
   * the red suits. Equivalently for the black suits (SuitType::kSpades and
   * SuitType::kClubs). The last `SuitType` would be `SuitType::kNone` which
   * should only occur with empty tableau cards or hidden cards. Empty tableau
   * cards should accept any suit, but hidden cards are the opposite; they
   * shouldn't accept any. There isn't really a use case for calling this
   * function with the suit of a hidden card though. */

  switch (suit) {
    case SuitType::kSpades: {
      return {SuitType::kSpades, SuitType::kClubs};
    }
    case SuitType::kHearts: {
      return {SuitType::kHearts, SuitType::kDiamonds};
    }
    case SuitType::kClubs: {
      return {SuitType::kSpades, SuitType::kClubs};
    }
    case SuitType::kDiamonds: {
      return {SuitType::kHearts, SuitType::kDiamonds};
    }
    case SuitType::kNone: {
      return {SuitType::kSpades, SuitType::kHearts, SuitType::kClubs,
              SuitType::kDiamonds};
    }
    default: {
      SpielFatalError("suit is not in (s, h, c, d)");
    }
  }
}

int GetCardIndex(RankType rank, SuitType suit) {
  /* Using a given rank and/or suit, gets an integer representing the index
   * of the card. */

  if (rank == RankType::kHidden || suit == SuitType::kHidden) {
    // Handles hidden_ cards_
    return kHiddenCard;
  } else if (rank == RankType::kNone) {
    // Handles special cards_
    if (suit == SuitType::kNone) {
      // Handles empty tableau cards_
      return kEmptyTableauCard;
    } else {
      // Handles empty foundation cards
      switch (suit) {
        case SuitType::kSpades: {
          return kEmptySpadeCard;
        }
        case SuitType::kHearts: {
          return kEmptyHeartCard;
        }
        case SuitType::kClubs: {
          return kEmptyClubCard;
        }
        case SuitType::kDiamonds: {
          return kEmptyDiamondCard;
        }
        default: {
          SpielFatalError("Failed to get card index_");
        }
      }
    }
  } else {
    // Handles ordinary cards (e.g. 0-13 -> spades, 14-26 -> hearts, etc.)
    return (static_cast<int>(suit) - 1) * kNumRanks + static_cast<int>(rank);
  }
}

int GetMaxSize(LocationType location) {
  if (location >= LocationType::kDeck && location <= LocationType::kWaste) {
    // Cards can only be removed from the waste_&  there are 23 cards_ in it
    // at the start of the game
    return kMaxSizeWaste;
  } else if (location == LocationType::kFoundation) {
    // There are 13 cards_ in a suit_
    return kMaxSizeFoundation;
  } else if (location == LocationType::kTableau) {
    // There are a maximum of 0 hidden cards and 26 non-hidden cards in a
    // tableau (2 for each rank, all of the same color)
    return kMaxSizeTableau;
  } else {
    return 0;
  }
}

std::hash<std::string> hasher;

// Card Methods ================================================================

Card::Card(bool hidden, SuitType suit, RankType rank, LocationType location)
    : rank_(rank), suit_(suit), location_(location), hidden_(hidden) {}

Card::Card(int index, bool hidden, LocationType location)
    : location_(location), hidden_(hidden), index_(index) {
  if (!hidden_) {
    switch (index_) {
      case kHiddenCard: {
        rank_ = RankType::kHidden;
        suit_ = SuitType::kHidden;
        break;
      }
      case kEmptyTableauCard: {
        rank_ = RankType::kNone;
        suit_ = SuitType::kNone;
        break;
      }
      case kEmptySpadeCard: {
        rank_ = RankType::kNone;
        suit_ = SuitType::kSpades;
        break;
      }
      case kEmptyHeartCard: {
        rank_ = RankType::kNone;
        suit_ = SuitType::kHearts;
        break;
      }
      case kEmptyClubCard: {
        rank_ = RankType::kNone;
        suit_ = SuitType::kClubs;
        break;
      }
      case kEmptyDiamondCard: {
        rank_ = RankType::kNone;
        suit_ = SuitType::kDiamonds;
        break;
      }
      default: {
        // Converts an index back into a rank and suit for ordinary cards
        rank_ = static_cast<RankType>(1 + ((index_ - 1) % kNumRanks));
        suit_ = static_cast<SuitType>(
            static_cast<int>(1 + floor((index_ - 1) / 13.0)));
      }
    }
  }
}

// Getters

RankType Card::GetRank() const { return rank_; }

SuitType Card::GetSuit() const { return suit_; }

LocationType Card::GetLocation() const { return location_; }

bool Card::GetHidden() const { return hidden_; }

int Card::GetIndex() const {
  /* Basically it just calculates the index if it hasn't been calculated before,
   * otherwise it will just return a stored value. If `force` is true and the
   * card isn't hidden, then the index is calculated again. */
  return hidden_ ? kHiddenCard : GetCardIndex(rank_, suit_);
}

// Setters

void Card::SetRank(RankType new_rank) { rank_ = new_rank; }

void Card::SetSuit(SuitType new_suit) { suit_ = new_suit; }

void Card::SetLocation(LocationType new_location) { location_ = new_location; }

void Card::SetHidden(bool new_hidden) { hidden_ = new_hidden; }

// Other Methods

std::string Card::ToString(bool colored) const {
  std::string result;

  // Determine color of string
  if (colored && !hidden_) {
    if (suit_ == SuitType::kSpades || suit_ == SuitType::kClubs) {
      absl::StrAppend(&result, kBlack);
    } else if (suit_ == SuitType::kHearts || suit_ == SuitType::kDiamonds) {
      absl::StrAppend(&result, kRed);
    }
  }

  // Determine contents of string
  if (rank_ == RankType::kHidden || suit_ == SuitType::kHidden) {
    absl::StrAppend(&result, kGlyphHidden, " ");
  } else if (rank_ == RankType::kNone && suit_ == SuitType::kNone) {
    absl::StrAppend(&result, kGlyphEmpty);
  } else {
    absl::StrAppend(&result, kRankStrs.at(static_cast<int>(rank_)));
    absl::StrAppend(&result, kSuitStrs.at(static_cast<int>(suit_)));
  }

  if (colored) {
    // Reset color if applicable
    absl::StrAppend(&result, kReset);
  }

  return result;
}

std::vector<Card> Card::LegalChildren() const {
  if (hidden_) {
    return {};
  } else {
    std::vector<RankType> child_ranks;
    std::vector<SuitType> child_suits;

    // An empty tableau accepts any card (maximum of 52 children)
    // And any card accepts two cards of rank one less
    child_suits.reserve(4);
    child_ranks.reserve(13);

    switch (location_) {
      case LocationType::kTableau: {
        if (rank_ == RankType::kNone) {
          if (suit_ == SuitType::kNone) {
            // Empty tableaus can accept any card
            child_ranks = kRanks;
            child_suits = kSuits;
            break;
          } else {
            return {};
          }
        } else if (rank_ >= RankType::k2 && rank_ <= RankType::kK) {
          // Cards can accept cards of a same color suit that is one 
          // rank lower
          child_ranks.push_back(static_cast<RankType>(static_cast<int>(rank_) - 1));
          child_suits = GetSameColorSuits(suit_);
          break;
        } else if (rank_ == RankType::kA) {
          // Aces accepts Ks of a same color suit (Turn the corner)
          child_ranks.push_back(RankType::kK);
          child_suits = GetSameColorSuits(suit_);
          break;
        } else {
          // This will catch RankType::kHidden
          return {};
        }
        break;
      }
      case LocationType::kFoundation: {
        // OBS: foundation rank, see below, maybe it's good to have an error
        // throwing here
        return {};
        break;
      }
      default: {
        // This catches all cards_ that aren't located in a tableau or
        // foundation
        return {};
      }
    }

    std::vector<Card> legal_children;
    legal_children.reserve(52);

    if (child_suits.empty()) {
      SpielFatalError("child_suits should not be empty");
    }

    for (const auto& child_suit : child_suits) {
      for (const auto& child_rank : child_ranks) {
        auto child = Card(false, child_suit, child_rank);
        legal_children.push_back(child);
      }
    }

    return legal_children;
  }
}

std::vector<Card> Card::LegalChildren(RankType foundation_rank) const {
  if (hidden_) {
    return {};
  } else {

    if (foundation_rank == RankType::kHidden) {
      SpielFatalError("foundation rank should not be hidden");
    }

    RankType child_rank;
    std::vector<SuitType> child_suits;

    // An empty tableau accepts any card (maximum of 52 children)
    // And any card accepts two cards of rank one less
    child_suits.reserve(4);

    switch (location_) {
      case LocationType::kTableau: {
        // for cards in tableau, ignore foundation_rank if given
        return LegalChildren();
      }
      case LocationType::kFoundation: {
        if (foundation_rank == RankType::kNone) {
          return {};
        } else if (rank_ == RankType::kNone) {
          // if there's no card in a foundation, accept cards with rank_ == foundation_rank
          if (suit_ != SuitType::kNone) {
            child_rank  = foundation_rank;
            child_suits = {suit_};
          } else {
            return {};
          }
        } else if (rank_ >= RankType::kA && rank_ <= RankType::kQ) {
          // Accept a card of the same suit that is one rank higher
          child_rank = static_cast<RankType>(static_cast<int>(rank_) + 1);
          child_suits = {suit_};
        } else if (rank_ == RankType::kK) {
          // Accept Ace (turn the corner)
          child_rank = RankType::kA;
          child_suits = {suit_};
        } else {
          // Should not run
          return {};
        }
        break;
      }
      default: {
        // This catches all cards_ that aren't located in a tableau or
        // foundation
        return {};
      }
    }

    std::vector<Card> legal_children;
    legal_children.reserve(4);

    if (child_suits.empty()) {
      SpielFatalError("child_suits should not be empty");
    }

    for (const auto& child_suit : child_suits) {
      auto child = Card(false, child_suit, child_rank);
      legal_children.push_back(child);
    }

    return legal_children;
  }
}

bool Card::operator==(const Card& other_card) const {
  return rank_ == other_card.rank_ && suit_ == other_card.suit_;
}

bool Card::operator<(const Card& other_card) const {
  if (suit_ != other_card.suit_) {
    return suit_ < other_card.suit_;
  } else if (rank_ != other_card.rank_) {
    return rank_ < other_card.rank_;
  } else {
    return false;
  }
}

// Pile Methods ================================================================

Pile::Pile(LocationType type, PileID id, SuitType suit)
    : type_(type), suit_(suit), id_(id), max_size_(GetMaxSize(type)) {
  cards_.reserve(max_size_);
}

// Getters/Setters

bool Pile::GetIsEmpty() const { return cards_.empty(); }

Card Pile::GetFirstCard() const { return cards_.front(); }

Card Pile::GetLastCard() const { return cards_.back(); }

SuitType Pile::GetSuit() const { return suit_; }

LocationType Pile::GetType() const { return type_; }

PileID Pile::GetID() const { return id_; }

std::vector<Card> Pile::GetCards() const { return cards_; }

void Pile::SetCards(std::vector<Card> new_cards) {
  cards_ = std::move(new_cards);
}

// Other Methods

std::vector<Card> Pile::Targets() const {
  std::cout << "Pile::Targets()" << std::endl;
  switch (type_) {
    case LocationType::kFoundation: {
      if (!cards_.empty()) {
        return {cards_.back()};
      } else {
        // Empty foundation card with the same suit as the pile
        return {Card(false, suit_, RankType::kNone, LocationType::kFoundation)};
      }
    }
    case LocationType::kTableau: {
      if (!cards_.empty()) {
        auto back_card = cards_.back();
        if (!back_card.GetHidden()) {
          return {cards_.back()};
        } else {
          return {};
        }
      } else {
        // Empty tableau card (no rank or suit)
        return {Card(false, SuitType::kNone, RankType::kNone,
                     LocationType::kTableau)};
      }
    }
    default: {
      SpielFatalError("Pile::Targets() called with unsupported type_");
    }
  }
}

std::vector<Card> Pile::Sources() const {
  std::cout << "Pile::Targets()" << std::endl;
  std::vector<Card> sources;
  // A pile can have a maximum of 26 cards as sources (2 for each rank, all of the same color)
  sources.reserve(2*kNumRanks);
  switch (type_) {
    case LocationType::kFoundation: {
      if (!cards_.empty()) {
        return {cards_.back()};
      } else {
        return {};
      }
    }
    case LocationType::kTableau: {
      if (!cards_.empty()) {
        for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
          const auto& card = *it;
          auto prev_card = *cards_.rbegin();
          if (card.GetHidden()) {
            break;
          }
          if (card == *cards_.rbegin()) {
            sources.push_back(card);
          } else {
            auto children = card.LegalChildren();
            if (std::find(children.begin(), children.end(),
                prev_card) != children.end()) {
              sources.push_back(card);
              prev_card = card;
            } else {
              break;
            }
          } 
        }
      return sources;
      } else {
        return {};
      }
      break;
    }
    case LocationType::kWaste: {
      if (!cards_.empty()) {
        for (const auto& card : cards_) {
          if (!card.GetHidden()) {
            // All revealed cards are sources
            // This only happens in the end of the game
            sources.push_back(card);
          } else {
            break;
          }
        }
        return sources;
      } else {
        return {};
      }
      break;
    }
    default: {
      SpielFatalError("Pile::Sources() called with unsupported type_");
    }
  }
}

std::vector<Card> Pile::Split(Card card) {
  std::vector<Card> split_cards;
  switch (type_) {
    case LocationType::kFoundation: {
      if (cards_.back() == card) {
        split_cards = {cards_.back()};
        cards_.pop_back();
      }
      break;
    }
    case LocationType::kTableau: {
      if (!cards_.empty()) {
        bool split_flag = false;
        for (auto it = cards_.begin(); it != cards_.end();) {
          if (*it == card) {
            split_flag = true;
          }
          if (split_flag) {
            split_cards.push_back(*it);
            it = cards_.erase(it);
          } else {
            ++it;
          }
        }
      }
      break;
    }
    case LocationType::kWaste: {
      if (!cards_.empty()) {
        for (auto it = cards_.begin(); it != cards_.end();) {
          if (*it == card) {
            split_cards.push_back(*it);
            it = cards_.erase(it);
            break;
          } else {
            ++it;
          }
        }
      }
      break;
    }
    default: {
      return {};
    }
  }
  return split_cards;
}

void Pile::Reveal(Card card_to_reveal) {
  SpielFatalError("Pile::Reveal() is not implemented.");
}

void Pile::Extend(std::vector<Card> source_cards) {
  for (auto& card : source_cards) {
    card.SetLocation(type_);
    cards_.push_back(card);
  }
}

std::string Pile::ToString(bool colored) const {
  std::string result;
  for (const auto& card : cards_) {
    absl::StrAppend(&result, card.ToString(colored), " ");
  }
  return result;
}

// Tableau Methods =============================================================

Tableau::Tableau(PileID id)
    : Pile(LocationType::kTableau, id, SuitType::kNone) {}

std::vector<Card> Tableau::Targets() const {
  if (!cards_.empty()) {
    auto back_card = cards_.back();
    if (!back_card.GetHidden()) {
      return {cards_.back()};
    } else {
      return {};
    }
  } else {
    // Empty tableau card (no rank or suit)
    return {
        Card(false, SuitType::kNone, RankType::kNone, LocationType::kTableau)};
  }
}

std::vector<Card> Tableau::Sources() const {
  std::vector<Card> sources;
  sources.reserve(kMaxSourcesTableau);
  if (!cards_.empty()) {
    for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
      const auto& card = *it;
      auto prev_card = *cards_.rbegin();
      if (card.GetHidden()) {
        break;
      }
      if (card == *cards_.rbegin()) {
        sources.push_back(card);
      } else {
        auto children = card.LegalChildren();
        if (std::find(children.begin(), children.end(), prev_card) != children.end()) {
          sources.push_back(card);
          prev_card = card;
        } else {
          break;
        }
      }
    }
    return sources;
  } else {
    return {};
  }
}

std::vector<Card> Tableau::Split(Card card) {
  std::vector<Card> split_cards;
  if (!cards_.empty()) {
    bool split_flag = false;
    for (auto it = cards_.begin(); it != cards_.end();) {
      if (*it == card) {
        split_flag = true;
      }
      if (split_flag) {
        split_cards.push_back(*it);
        it = cards_.erase(it);
      } else {
        ++it;
      }
    }
  }
  return split_cards;
}

void Tableau::Reveal(Card card_to_reveal) {
  for (auto& card : cards_) {
    if (card.GetHidden()) {
      card.SetRank(card_to_reveal.GetRank());
      card.SetSuit(card_to_reveal.GetSuit());
      card.SetHidden(false);
      break;
    }
  }
}

// Foundation Methods ==========================================================

Foundation::Foundation(PileID id, SuitType suit)
    : Pile(LocationType::kFoundation, id, suit) {}

std::vector<Card> Foundation::Targets() const {
  if (!cards_.empty()) {
    return {cards_.back()};
  } else {
    // Empty foundation card with the same suit as the pile
    return {Card(false, suit_, RankType::kNone, LocationType::kFoundation)};
  }
}

std::vector<Card> Foundation::Sources() const {
  std::vector<Card> sources;
  sources.reserve(kMaxSourcesFoundation);
  if (!cards_.empty()) {
    return {cards_.back()};
  } else {
    return {};
  }
}

std::vector<Card> Foundation::Split(Card card) {
  std::vector<Card> split_cards;
  if (cards_.back() == card) {
    split_cards = {cards_.back()};
    cards_.pop_back();
  }
  return split_cards;
}

// Waste Methods ===============================================================

Waste::Waste() : Pile(LocationType::kWaste, PileID::kWaste, SuitType::kNone) {}

std::vector<Card> Waste::Targets() const { return {}; }

std::vector<Card> Waste::Sources() const {
  std::vector<Card> sources;
  sources.reserve(kMaxSourcesWaste);
  if (!cards_.empty()) {
    for (const auto& card : cards_) {
      if (!card.GetHidden()) {
        sources.push_back(card);
      } else {
        break;
      }
    }
    return sources;
  } else {
    return {};
  }
}

std::vector<Card> Waste::Split(Card card) {
  std::vector<Card> split_cards;
  if (!cards_.empty()) {
    for (auto it = cards_.begin(); it != cards_.end();) {
      if (*it == card) {
        split_cards.push_back(*it);
        it = cards_.erase(it);
        break;
      } else {
        ++it;
      }
    }
  }
  return split_cards;
}

void Waste::Reveal(Card card_to_reveal) {
  for (auto& card : cards_) {
    if (card.GetHidden()) {
      card.SetRank(card_to_reveal.GetRank());
      card.SetSuit(card_to_reveal.GetSuit());
      card.SetHidden(false);
      break;
    }
  }
}

// Move Methods ================================================================

Move::Move(Card target_card, Card source_card) {
  target_ = target_card;
  source_ = source_card;
}

Move::Move(RankType target_rank, SuitType target_suit, RankType source_rank,
           SuitType source_suit) {
  target_ = Card(false, target_suit, target_rank, LocationType::kMissing);
  source_ = Card(false, source_suit, source_rank, LocationType::kMissing);
}

Move::Move(Action action) {

  int target_rank;
  int source_rank;
  int target_suit;
  int source_suit;

  action -= kActionOffset;

  // The numbers used in the cases below are just used to divide
  // action ids into groups

  if (action >= 1 && action <= 52) {
    // Handles card to empty foundation
    source_rank = (action-1)%13+1;
    source_suit = ((action-1)-(source_rank-1))/13+1;
    target_rank = 0;
    target_suit = source_suit;
  } else if (action >= 53 && action <= 100) {
    // Handles card (not A) to not empty foundation
    source_rank = (action-53)%12+2;
    source_suit = ((action-53)-(source_rank-2))/12+1;
    target_rank = source_rank-1;
    target_suit = source_suit;
  } else if (action >= 101 && action <= 104) {
    // Handles A on top of K in foundation
    source_rank = 1;
    source_suit = action-(101-1);
    target_rank = 13;
    target_suit = source_suit;
  } else if (action >= 105 && action <= 152) {
    // Handles card (not K) to tableau (same suit)
    source_rank = (action-105)%12+1;
    source_suit = ((action-105)-(source_rank-1))/12+1;
    target_rank = source_rank+1;
    target_suit = source_suit;
  } else if (action >= 153 && action <= 156) {
    // Handles K to A on tableau (same suit)
    source_rank = 13;
    source_suit = (action-153)/4+1;
    target_rank = 1;
    target_suit = source_suit;
  } else if (action >= 157 && action <= 204) {
    // Handles card (not K) to tableau (opposite suit)
    source_rank = (action-157)%12+1;
    source_suit = ((action-157)-(source_rank-1))/12+1;
    target_rank = source_rank+1;
    target_suit = (source_suit+1)%4+1;
  } else if (action >= 205 && action <= 208) {
    // Handles K to tableau (opposite suit)
    source_rank = 13;
    source_suit = (action-205)/4+1;
    target_rank = source_rank+1;
    target_suit = (source_suit+1)%4+1;
  } else if (action >= 209 && action <= 260) {
    // Handles card to empty tableau
    target_rank = 0;
    target_suit = 0;
    source_rank = (action-209)%13+1;
    source_suit = ((action-209)-(source_rank-1))/13+1;
  } else if (action >= 261 && action <= 312) {
    // Handles hidden card to tableau
    target_rank = (action-261)%13+1;
    target_suit = ((action-261)-(target_rank-1))/13+1;
    source_rank = 14;
    source_suit = 5;
  } else if (action == 313) {
    // Handles hidden card to empty tableau
    target_rank = 0;
    target_suit = 0;
    source_rank = 14;
    source_suit = 5;
  } else {
    SpielFatalError("action provided does not correspond with a move");
  }

  target_ = Card(false, static_cast<SuitType>(target_suit),
                 static_cast<RankType>(target_rank));
  source_ = Card(false, static_cast<SuitType>(source_suit),
                 static_cast<RankType>(source_rank));
}

// Getters

Card Move::GetTarget() const { return target_; }

Card Move::GetSource() const { return source_; }

// Other Methods

Action Move::ActionId() const {
  int target_rank = static_cast<int>(target_.GetRank());
  int source_rank = static_cast<int>(source_.GetRank());
  int target_suit = static_cast<int>(target_.GetSuit());
  int source_suit = static_cast<int>(source_.GetSuit());

  int offset = 52;

  int base;

  // Handle all cases with hidden or none cards
  if (target_rank == 0 && target_suit == 0 && source_rank == 14 &&
      source_suit == 5) {
    // Handles hidden card to empty tableau (waste -> tableau)
    return 313+offset;
  } else if (source_rank == 14 && source_suit == 5 && target_rank != 0 &&
             target_rank != 14 && target_suit != 0 && target_suit != 5) {
    // Handles hidden card to tableau (waste -> tableau)
    base = 261+offset;
    return base + (target_suit - 1) * 13 + (target_rank - 1);
  } else if (target_rank == 0 && target_suit == 0 && source_rank != 0 &&
             source_rank != 14 && source_suit != 0 && source_suit != 5) {
    // Handles card to empty tableau
    base = 209+offset;
    return base + (source_suit - 1) * 13 + (source_rank - 1);
  } else if (target_rank == 0 && target_suit != 0 && target_suit != 5 &&
             source_rank != 0 && source_rank != 14 && source_suit != 0 &&
             source_suit != 5) {
    // Handles card to empty foundation
    base = 1 + offset;
    return base + (source_suit - 1) * 13 + (source_rank - 1);
  }

  // Handle all cases without hidden or unknown cards
  if (target_rank != 0 && target_rank != 14 && source_rank != 0 &&
      source_rank != 14 && target_suit != 0 && target_suit != 5 &&
      source_suit != 0 & source_suit != 5) {

    if (source_rank == 13 && target_rank == 1 && abs(target_suit - source_suit)%4 == 2) {
      // Handles K to A on tableau (opposite suit)
      base = 205+offset;
      return base + (source_suit - 1);
    } else if (target_rank - source_rank == 1 && abs(target_suit - source_suit)%4 == 2) {
      // Handles card (not K) to tableau (opposite suit)
      base = 157+offset;
      return base + (source_suit-1) * 12 + (source_rank-1);
    } else if (target_rank == 1 && source_rank == 13 &&
              target_suit == source_suit) {
      // Handles K to A on tableau (same suit)
      base = 153+offset;
      return base + (source_suit - 1);
    } else if (target_rank - source_rank == 1 && target_suit == source_suit) {
      // Handles card (not K) to tableau (same suit)
      base = 105+offset;
      return base + (source_suit-1) * 12 + (source_rank-1);
    } else if (source_rank == 1 && target_rank == 13 &&
              target_suit == source_suit) {
      // Handles A on top of K in foundation
      base = 101+offset;
      return base + (source_suit - 1);
    } else if (target_suit == source_suit && source_rank - target_rank == 1) {
      // Handles card (not A) to foundation
      base = 53+offset;
      return base + (source_suit-1) * 12 + (source_rank-2);
    } else {
      SpielFatalError("move not found");
      return -999; // see solitaire.cc line 894
    }
  } else {
    SpielFatalError("move not found");
    return -999; // see solitaire.cc line 894
  }

}

std::string Move::ToString(bool colored) const {
  std::string result;
  absl::StrAppend(&result, target_.ToString(colored), " ", kGlyphArrow, " ",
                  source_.ToString(colored));
  return result;
}

bool Move::operator<(const Move& other_move) const {
  int index_ = target_.GetIndex() * 100 + source_.GetIndex();
  int other_index =
      other_move.target_.GetIndex() * 100 + other_move.source_.GetIndex();
  return index_ < other_index;
}

// AgnesSorelState Methods ======================================================

AgnesSorelState::AgnesSorelState(std::shared_ptr<const Game> game)
    : State(game), waste_() {
  // Extract parameters from `game`
  auto parameters = game->GetParameters();
  is_colored_ = parameters.at("is_colored").bool_value();
  depth_limit_ = parameters.at("depth_limit").int_value();

  // Create foundations_
  for (const auto& suit_ : kSuits) {
    foundations_.emplace_back(kSuitToPile.at(suit_), suit_);
  }

  // Create tableaus_
  for (int i = 1; i <= 7; i++) {
    std::vector<Card> cards_to_add;
    for (int j = 1; j <= i; j++) {
      cards_to_add.emplace_back(true, SuitType::kHidden, RankType::kHidden,
                                LocationType::kTableau);
    }

    // Create a new tableau and add cards_
    auto tableau = Tableau(kIntToPile.at(i));
    tableau.SetCards(cards_to_add);

    // Add resulting tableau to tableaus_
    tableaus_.push_back(tableau);
  }

  // Create waste_
  for (int i = 1; i <= 23; i++) {
    auto new_card =
        Card(true, SuitType::kHidden, RankType::kHidden, LocationType::kWaste);
    waste_.Extend({new_card});
  }
}

Player AgnesSorelState::CurrentPlayer() const {
  if (IsTerminal()) {
    return kTerminalPlayerId;
  } else if (IsChanceNode()) {
    return kChancePlayerId;
  } else {
    return kPlayerId;
  }
}

std::unique_ptr<State> AgnesSorelState::Clone() const {
  return std::unique_ptr<State>(new AgnesSorelState(*this));
}

bool AgnesSorelState::IsKnownFoundation() const { return is_known_foundation_; }

bool AgnesSorelState::IsTerminal() const { return is_finished_; }

bool AgnesSorelState::IsChanceNode() const {
  // IsChanceNode if any card in tableau is hidden
  // This happens at the start of game and after a new
  // row is dealt from the waste to the tableau
  for (const auto& tableau : tableaus_) {
    if (!tableau.GetIsEmpty() ) {
      for (const auto& card : tableau.GetCards() ) {
        if (card.GetHidden()) {
          return true;
        }
      }
    }
  }
  if (!is_known_foundation_) {
    return true;
  }
  return false;
}

std::string AgnesSorelState::ToString() const {
  std::string result;

  absl::StrAppend(&result, "WASTE       : ", waste_.ToString(is_colored_));

  absl::StrAppend(&result, "\nFOUNDATIONS : ");
  for (const auto& foundation : foundations_) {
    absl::StrAppend(&result, foundation.Targets()[0].ToString(is_colored_),
                    " ");
  }

  absl::StrAppend(&result, "\nTABLEAUS    : ");
  for (const auto& tableau : tableaus_) {
    if (!tableau.GetIsEmpty()) {
      absl::StrAppend(&result, "\n", tableau.ToString(is_colored_));
    }
  }

  absl::StrAppend(&result, "\nTARGETS : ");
  for (const auto& card : Targets()) {
    absl::StrAppend(&result, card.ToString(is_colored_), " ");
  }

  absl::StrAppend(&result, "\nSOURCES : ");
  for (const auto& card : Sources()) {
    absl::StrAppend(&result, card.ToString(is_colored_), " ");
  }

  return result;
}

std::string AgnesSorelState::ActionToString(Player player,
                                           Action action_id) const {
  if (action_id == kEnd) {
    return "kEnd";
  } else if (action_id >= kRevealStart && action_id <= kRevealEnd) {
    auto revealed_card = Card(static_cast<int>(action_id));
    std::string result;
    absl::StrAppend(&result, "Reveal", revealed_card.ToString(is_colored_));
    return result;
  } else if (action_id >= kMoveStart && action_id <= kMoveEnd) {
    auto move = Move(action_id);
    return move.ToString(is_colored_);
  } else if (action_id == kDeal) {
    std::string result;
    absl::StrAppend(&result, "Deal/Reveal from waste");
    return result;
  } else {
    return "Missing Action";
  }
}

std::string AgnesSorelState::InformationStateString(Player player) const {
  SPIEL_CHECK_GE(player, 0);
  SPIEL_CHECK_LT(player, num_players_);
  return HistoryString();
}

std::string AgnesSorelState::ObservationString(Player player) const {
  SPIEL_CHECK_GE(player, 0);
  SPIEL_CHECK_LT(player, num_players_);
  return ToString();
}

void AgnesSorelState::ObservationTensor(Player player,
                                       absl::Span<float> values) const {
                                        // TODO: fix
  SPIEL_CHECK_GE(player, 0);
  SPIEL_CHECK_LT(player, num_players_);

  SPIEL_CHECK_EQ(values.size(), game_->ObservationTensorSize());
  std::fill(values.begin(), values.end(), 0.0);
  auto ptr = values.begin();

  for (const auto& foundation : foundations_) {
    if (foundation.GetIsEmpty()) {
      ptr[0] = 1;
    } else {
      auto last_rank = foundation.GetLastCard().GetRank();
      if (last_rank >= RankType::kA && last_rank <= RankType::kK) {
        ptr[static_cast<int>(last_rank)] = 1;
      }
    }
    ptr += kFoundationTensorLength;
  }

  for (const auto& tableau : tableaus_) {
    if (tableau.GetIsEmpty()) {
      ptr[7] = 1.0;
    } else {
      int num_hidden_cards = 0;
      for (const auto& card : tableau.GetCards()) {
        if (card.GetHidden() && num_hidden_cards <= kMaxHiddenCard) {
          ptr[num_hidden_cards] = 1.0;
          ++num_hidden_cards;
        } else {
          auto tensor_index = card.GetIndex() + kMaxHiddenCard;
          ptr[tensor_index] = 1.0;
        }
      }
    }
    ptr += kTableauTensorLength;
  }

  for (auto& card : waste_.GetCards()) {
    if (card.GetHidden()) {
      ptr[0] = 1.0;
    } else {
      auto tensor_index = card.GetIndex();
      ptr[tensor_index] = 1.0;
    }
    ptr += kWasteTensorLength;
  }

  SPIEL_CHECK_LE(ptr, values.end());
}

void AgnesSorelState::DoApplyAction(Action action) {
  if (action == kEnd) {
    is_finished_ = true;
    current_rewards_ = 0;
  } else if (action >= kRevealStart && action <= kRevealEnd) {
    auto revealed_card = Card(static_cast<int>(action));
    bool found_card = false;

    for (auto& tableau : tableaus_) {
      if (!tableau.GetIsEmpty()) {
        for (auto& card : tableau.GetCards()) {
          if (card.GetHidden()) {
            tableau.Reveal(revealed_card);
            card_map_.insert_or_assign(revealed_card, tableau.GetID());
            found_card = true;
            break;
          }
        }
      }
      if (found_card) {
        break;
      }
    }
    if (!is_known_foundation_ && !found_card) {
      // add 29th card to foundation
      foundation_rank_     = revealed_card.GetRank();
      is_known_foundation_ = true;
      auto pile_id = kSuitToPile.at(revealed_card.GetSuit());
      for (auto& foundation : foundations_) {
        if (foundation.GetSuit() == revealed_card.GetSuit()) {
          foundation.Extend({revealed_card});
        }
      }
      card_map_.insert_or_assign(revealed_card, pile_id);
    }
    revealed_cards_.push_back(action);
  } else if (action >= kMoveStart && action <= kMoveEnd) {
    Move selected_move = Move(action);
    is_reversible_ = IsReversible(selected_move.GetSource(),
                                  GetPile(selected_move.GetSource()),
                                  selected_move.GetTarget(),
                                  GetPile(selected_move.GetTarget()));

    if (is_reversible_) {
      std::string current_observation = ObservationString(0);
      previous_states_.insert(hasher(current_observation));
    } else {
      previous_states_.clear();
    }

    MoveCards(selected_move);
    current_returns_ += current_rewards_;
  } else if (action == kDeal) {
    if (waste_.GetIsEmpty()) {
      SpielFatalError("kDeal is not a valid move when waste is empty");
    }
    auto waste_cards  = waste_.GetCards();
    if ( waste_cards.size() >= 7 ) {
      // deal 7 cards from waste to tableau
      for (auto& tableau : tableaus_) {
        std::vector<Card> split_cards = waste_.Split(waste_.GetLastCard());
        for (auto& card : split_cards) {
          card_map_.insert_or_assign(card, tableau.GetID());
          tableau.Extend({card});
        }
      }
      is_reversible_ = false;
      previous_states_.clear();
    } else {
      // deal 2 cards from waste to tableau
      int waste_size = waste_cards.size();
      for (auto it = tableaus_.begin(); it != tableaus_.end() &&
          std::distance(tableaus_.begin(), it) < waste_size; ++it) {
        auto& tableau = *it;
        std::vector<Card> split_cards = waste_.Split(waste_.GetLastCard());
        for (auto& card : split_cards) {
          card_map_.insert_or_assign(card, tableau.GetID());
          tableau.Extend({card});
        }
      }
    }
  }
  ++current_depth_;
  if (current_depth_ >= depth_limit_) {
    is_finished_ = true;
  }
}

std::vector<double> AgnesSorelState::Returns() const {
  // Returns the sum of rewards up to and including the most recent state
  // transition.
  return {current_returns_};
}

std::vector<double> AgnesSorelState::Rewards() const {
  // Should be the reward for the action that created this state, not the action
  // applied to this state
  return {current_rewards_};
}

std::vector<Action> AgnesSorelState::LegalActions() const { // TODO: fix
  if (IsTerminal()) {
    return {};
  } else if (IsChanceNode()) {
    return LegalChanceOutcomes();
  } else {
    std::vector<Action> legal_actions;

    if (is_reversible_) {
      // If the state is reversible, we need to check each move to see if it is
      // too.
      for (const auto& move : CandidateMoves()) {
        if (IsReversible(move.GetSource(), GetPile(move.GetSource()),
                        move.GetTarget(),GetPile(move.GetTarget()))) {
          std::cerr << "test 1" << std::endl; // TODO: REMOVE
          auto action_id = move.ActionId();
          std::cerr << "test 2" << std::endl; // TODO: REMOVE
          auto child = Child(action_id);

          if (child->CurrentPlayer() == kChancePlayerId) {
            legal_actions.push_back(action_id);
          } else {
            auto child_hash = hasher(child->ObservationString());
            if (previous_states_.count(child_hash) == 0) {
              legal_actions.push_back(action_id);
            }
          }
        } else {
          std::cerr << "test 3" << std::endl; // TODO: REMOVE
          legal_actions.push_back(move.ActionId());
          std::cerr << "test 4" << std::endl; // TODO: REMOVE
        }
      }
    } else {
      // If the state isn't reversible, all candidate moves are legal
      for (const auto& move : CandidateMoves()) {
        legal_actions.push_back(move.ActionId());
      }
    }

    if (!waste_.GetIsEmpty()) {
      legal_actions.push_back(kDeal);
    }
    if (!legal_actions.empty()) {
      std::sort(legal_actions.begin(), legal_actions.end());
    } else {
      legal_actions.push_back(kEnd);
    }
    return legal_actions;
  }
}

std::vector<std::pair<Action, double>> AgnesSorelState::ChanceOutcomes() const {
  std::vector<std::pair<Action, double>> outcomes;
  const double p = 1.0 / (52 - revealed_cards_.size());

  for (int i = 1; i <= 52; i++) {
    if (std::find(revealed_cards_.begin(), revealed_cards_.end(), i) ==
        revealed_cards_.end()) {
      outcomes.emplace_back(i, p);
    }
  }

  return outcomes;
}

// Other Methods

std::vector<Card> AgnesSorelState::Targets(
    const absl::optional<LocationType>& location) const {
  LocationType loc = location.value_or(LocationType::kMissing);
  std::vector<Card> targets;

  if (loc == LocationType::kTableau || loc == LocationType::kMissing) {
    for (const auto& tableau : tableaus_) {
      std::vector<Card> current_targets = tableau.Targets();
      targets.insert(targets.end(), current_targets.begin(),
                     current_targets.end());
    }
  }

  if (loc == LocationType::kFoundation || loc == LocationType::kMissing) {
    for (const auto& foundation : foundations_) {
      std::vector<Card> current_targets = foundation.Targets();
      targets.insert(targets.end(), current_targets.begin(),
                     current_targets.end());
    }
  }

  return targets;
}

std::vector<Card> AgnesSorelState::Sources(
    const absl::optional<LocationType>& location) const {
  LocationType loc = location.value_or(LocationType::kMissing);
  std::vector<Card> sources;

  if (loc == LocationType::kTableau || loc == LocationType::kMissing) {
    for (const auto& tableau : tableaus_) {
      std::vector<Card> current_sources = tableau.Sources();
      sources.insert(sources.end(), current_sources.begin(),
                     current_sources.end());
    }
  }

  if (loc == LocationType::kFoundation || loc == LocationType::kMissing) {
    for (const auto& foundation : foundations_) {
      std::vector<Card> current_sources = foundation.Sources();
      sources.insert(sources.end(), current_sources.begin(),
                     current_sources.end());
    }
  }

  if (loc == LocationType::kWaste || loc == LocationType::kMissing) {
    std::vector<Card> current_sources = waste_.Sources();
    sources.insert(sources.end(), current_sources.begin(),
                   current_sources.end());
  }

  return sources;
}

const Pile* AgnesSorelState::GetPile(const Card& card) const {
  PileID pile_id = PileID::kMissing;

  if (card.GetRank() == RankType::kNone) {
    if (card.GetSuit() == SuitType::kNone) {
      for (auto& tableau : tableaus_) {
        if (tableau.GetIsEmpty()) {
          return &tableau;
        }
      }
    } else if (card.GetSuit() != SuitType::kHidden) {
      for (auto& foundation : foundations_) {
        if (foundation.GetSuit() == card.GetSuit()) {
          return &foundation;
        }
      }
    } else {
      SpielFatalError("The pile containing the card wasn't found");
    }
  } else {
    pile_id = card_map_.at(card);
  }

  if (pile_id == PileID::kWaste) {
    return &waste_;
  } else if (pile_id >= PileID::kSpades && pile_id <= PileID::kDiamonds) {
    return &foundations_.at(static_cast<int>(pile_id) - 1);
  } else if (pile_id >= PileID::k1stTableau && pile_id <= PileID::k7thTableau) {
    return &tableaus_.at(static_cast<int>(pile_id) - 5);
  } else {
    SpielFatalError("The pile containing the card wasn't found");
  }
}

Pile* AgnesSorelState::GetPile(const Card& card) {
  PileID pile_id = PileID::kMissing;

  if (card.GetRank() == RankType::kNone) {
    if (card.GetSuit() == SuitType::kNone) {
      for (auto& tableau : tableaus_) {
        if (tableau.GetIsEmpty()) {
          return &tableau;
        }
      }
    } else if (card.GetSuit() != SuitType::kHidden) {
      for (auto& foundation : foundations_) {
        if (foundation.GetSuit() == card.GetSuit()) {
          return &foundation;
        }
      }
    } else {
      SpielFatalError("The pile containing the card wasn't found");
    }
  } else {
    pile_id = card_map_.at(card);
  }

  if (pile_id == PileID::kWaste) {
    return &waste_;
  } else if (pile_id >= PileID::kSpades && pile_id <= PileID::kDiamonds) {
    return &foundations_.at(static_cast<int>(pile_id) - 1);
  } else if (pile_id >= PileID::k1stTableau && pile_id <= PileID::k7thTableau) {
    return &tableaus_.at(static_cast<int>(pile_id) - 5);
  } else {
    SpielFatalError("The pile containing the card wasn't found");
  }
}

std::vector<Move> AgnesSorelState::CandidateMoves() const {
  std::vector<Move> candidate_moves;
  std::vector<Card> targets = Targets();
  std::vector<Card> sources = Sources();
  bool found_empty_tableau = false;

  for (auto& target : targets) {
    if (target.GetSuit() == SuitType::kNone &&
        target.GetRank() == RankType::kNone) {
      if (found_empty_tableau) {
        continue;
      } else {
        found_empty_tableau = true;
      }
    }
    for (auto& source : target.LegalChildren(foundation_rank_)) {
      if (std::find(sources.begin(), sources.end(), source) != sources.end()) {
        auto* source_pile = GetPile(source);
        if (target.GetLocation() == LocationType::kFoundation &&
            source_pile->GetType() == LocationType::kTableau) {
          if (source_pile->GetLastCard() == source) {
            candidate_moves.emplace_back(target, source);
          }
        // Any card to empty tableau
        } else if (target.GetSuit() == SuitType::kNone &&
                   target.GetRank() == RankType::kNone) {
          // Check if source is not a bottom
          if (source_pile->GetType() == LocationType::kWaste ||
              (source_pile->GetType() == LocationType::kTableau &&
               !(source_pile->GetFirstCard() == source))) {
            candidate_moves.emplace_back(target, source);
          }
        } else {
          candidate_moves.emplace_back(target, source);
        }
      } else {
        continue;
      }
    }
  }

  return candidate_moves;
}

void AgnesSorelState::MoveCards(const Move& move) {
  Card target = move.GetTarget();
  Card source = move.GetSource();

  auto* target_pile = GetPile(target);
  auto* source_pile = GetPile(source);

  std::vector<Card> split_cards = source_pile->Split(source);
  for (auto& card : split_cards) {
    card_map_.insert_or_assign(card, target_pile->GetID());
    target_pile->Extend({card});
  }

  // Calculate rewards/returns for this move in the current state
  double move_reward = 0.0;

  // Reward for moving a card to or from a foundation
  if (target_pile->GetType() == LocationType::kFoundation) {
    // Adds points for moving TO a foundation
    move_reward += kFoundationPoints.at(source.GetRank());
  } else if (source_pile->GetType() == LocationType::kFoundation) {
    // Subtracts points for moving AWAY from a foundation
    move_reward -= kFoundationPoints.at(source.GetRank());
  }

  // Reward for revealing a hidden_ card
  // TODO: remove this. or maybe not, if cards from the hidden pile also count to the goal
  if (source_pile->GetType() == LocationType::kTableau &&
      !source_pile->GetIsEmpty() && source_pile->GetLastCard().GetHidden()) {
    move_reward += 20.0;
  }

  // Reward for moving a card from the waste_
  if (source_pile->GetType() == LocationType::kWaste) {
    move_reward += 20.0;
  }

  // Add current rewards to current returns
  current_rewards_ = move_reward;
}

bool AgnesSorelState::IsReversible(const Card& source,
                                  const Pile* source_pile,
                                  const Card& target,
                                  const Pile* target_pile) const {
  switch (source.GetLocation()) {
    case LocationType::kWaste: {
      return false;
    }
    case LocationType::kFoundation: {
      return true;
    }
    case LocationType::kTableau: {
      // Reversible moves:
      // 1. From card with legal parent to empty tableau
      // e.g.: 4h over 3d moved to empty tableau
      // 2. From card with legal parent to other legal parent
      // e.g.: 4h over 3d moved to 3h
      // 3. From single card on a pile to an empty tableau
      // 4. From single card on a pile to a legal parent
      // e.g.: 4h (single) moved to 3h or 3d      
      // From single card on a pile
      if (source_pile->GetFirstCard() == source) {
        auto target_children = target.LegalChildren();
        if (std::find(target_children.begin(), target_children.end(),
            source) != target_children.end()) {
          return true;
        } else {
          return false;
        }
      // From card on a not single pile
      } else {
        // TODO: fix, not always the last card, if source is somewhere else
        auto source_children = source_pile->GetCards().rbegin()[1].LegalChildren();
        // children of second to last card in source pile
        if (std::find(source_children.begin(), source_children.end(),
            source) != source_children.end()) {
          auto target_children = target.LegalChildren();
          if (std::find(target_children.begin(), target_children.end(),
              source) != target_children.end()) {
            return true;
          } else {
            return false;
          }
        } else {
          return false;
        }
      }
      break;
    }
    default: {
      // Returns false if the source card is not in the waste, foundations,
      // or tableaus
      return false;
    }
  }
}

// AgnesSorelGame Methods =======================================================
// OK

AgnesSorelGame::AgnesSorelGame(const GameParameters& params)
    : Game(kGameType, params),
      num_players_(ParameterValue<int>("players")),
      depth_limit_(ParameterValue<int>("depth_limit")),
      is_colored_(ParameterValue<bool>("is_colored")) {}

int AgnesSorelGame::NumDistinctActions() const {
  /* 52 Reveal Moves (one for each ordinary card)
   * 52 Card to empty Foundation moves
   * 52 Card to Card on Foundation Moves
   * 104 Tableau Moves (two for every ordinary card)
   * e.g. 4h can be moved on top of 5h or 5d
   * 52 Card to Empty Tableau moves
   * 52 Hidden card from Waste to End of Tableau moves
   *  (card is always hidden here, so this is always the same move)
   *  1 Hidden card from waste to empty tableau
   *  1 Deal new row move
   *  1 End Game move
   * Total: 367 = 52 Reveal + 260 Move + 52 Deal + 1 Deal to empty tableau + 1 Player deal + 1 End */
  return 367;
}

int AgnesSorelGame::MaxChanceOutcomes() const { return kRevealEnd + 1; }

int AgnesSorelGame::MaxGameLength() const { return depth_limit_; }

int AgnesSorelGame::NumPlayers() const { return 1; }

double AgnesSorelGame::MinUtility() const {
  /* Returns start at zero and the only negative rewards come from undoing an
   * action. Undoing an action just takes away the reward that was gained from
   * the action, so utility can never go below 0. */
  return 0.0;
}

double AgnesSorelGame::MaxUtility() const {
  /* Waste (23 * 20 = 460)
     23 cards are in the waste initially. 20 points are rewarded for every one
     that is moved from the waste.
     Tableau (21 * 0 = 0)
     all cards are revealed in the tableaus_ from the start.
     TODO: But one could have points for the partial piles in the tableau
     Foundation (4 * (100 + 90 + 80 + 70 + 60 + 50 + 40 + 30 + 20 + 10
     + 10 + 10 + 10) - 100 = 4 * 580 - 100 = 2,220)
     1 card is in the foundations initially.
     A varying number of points, based on the cards rank, are
     awarded when the card is moved to the foundation. Each complete suit in
     the foundation is worth 580 points. `kFoundationPoints` in `agnes_sorel.h`
     outlines how much each rank is worth.
     Max Utility = 460 + 2,220 = 2,680 */
  return 2680.0;
}

std::vector<int> AgnesSorelGame::ObservationTensorShape() const {
  /* Waste (23 * 53 = 1,219)
     23 locations and each location_ is a 53 element vector (52 normal cards
     + 1 hidden)
     Tableau (7 * 53 = 371)
     Each tableau is represented as a 53 element vector
     (1 empty tableau + 52 normal cards_)
     Foundation (4 * 14 = 56)
     Each foundation is represented as a 14 element vector
     (13 ranks + 1 empty foundation)
     Foundation rank is 1 element
     Total Length = 1,219 + 371 + 56 + 1 = 1,647 */
  return {1647};
}

std::unique_ptr<State> AgnesSorelGame::NewInitialState() const {
  return std::unique_ptr<State>(new AgnesSorelState(shared_from_this()));
}

}  // namespace open_spiel::agnes_sorel
