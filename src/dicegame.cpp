#include "../include/dicegame.hpp"

void dicegame::launch(public_key pub_key, double casino_fee, double ref_bonus, double player_bonus)
{
    require_auth(SEVENSHELPER);

    auto stenv = tenvironments.get_or_default(environments{.locked = asset(0, EOS_SYMBOL), .next_id = 0});
    stenv.pub_key = pub_key;
    stenv.casino_fee = casino_fee;
    stenv.ref_bonus = ref_bonus;
    stenv.player_bonus = player_bonus;
    tenvironments.set(stenv, _self);
}

void dicegame::resolvebet(const uint64_t &bet_id, const signature &sig)
{
    require_auth(SEVENSHELPER);

    auto current_bet = tbets.find(bet_id);
    eosio_assert(current_bet != tbets.end(), "Bet doesn't exist");

    auto stenvironments = tenvironments.get();
    public_key key = stenvironments.pub_key;
    assert_recover_key(current_bet->house_seed_hash, sig, key);

    checksum256 sig_hash = sha256((char *)&sig, sizeof(sig));

    const uint64_t random_roll = get_random_roll(sig_hash);
    double fee = (double)stenvironments.casino_fee;
    asset payout = asset(0, EOS_SYMBOL);
    asset ref_bonus = asset(0, EOS_SYMBOL);

    if (!current_bet->referrer.to_string().empty())
    {
        fee -= stenvironments.player_bonus;
        ref_bonus.amount = current_bet->amount.amount * stenvironments.ref_bonus / 100;
    }

    asset possible_payout = calc_payout(current_bet->amount, current_bet->roll_under, fee);

    if (random_roll < current_bet->roll_under)
    {
        payout = possible_payout;
        action(
            permission_level{_self, name("active")},
            name("eosio.token"),
            name("transfer"),
            std::make_tuple(
                _self,
                current_bet->player,
                payout,
                winner_msg(current_bet->id)))
            .send();
    }

    unlock(possible_payout);

    resolvedBet result;
    result.id = current_bet->id;
    result.game_id = current_bet->game_id;
    result.player = current_bet->player;
    result.amount = current_bet->amount;
    result.roll_under = current_bet->roll_under;
    result.random_roll = random_roll;
    result.payout = payout;
    result.ref_payout = ref_bonus;
    result.player_seed = current_bet->player_seed;
    result.house_seed_hash = current_bet->house_seed_hash;
    result.sig = sig;
    result.referrer = current_bet->referrer;
    result.created_at = current_bet->created_at;

    SEND_INLINE_ACTION(*this, receipt, {_self, name("active")}, {result});

    if (ref_bonus.amount > 0)
    {
        transaction ref_trx{};
        ref_trx.actions.emplace_back(permission_level{_self, name("active")},
                                     _self,
                                     name("reftransfer"),
                                     std::make_tuple(current_bet->referrer, ref_bonus, ref_msg(current_bet->id)));
        ref_trx.delay_sec = 5;
        ref_trx.send(current_bet->id, _self);
    }

    airdrop_tokens(result.id, result.amount, result.player);

    tbets.erase(current_bet);

    tlogs.emplace(_self, [&](logs &l) {
        l.game_id = result.game_id;
        l.amount = result.amount;
        l.payout = result.payout;
        l.random_roll = result.random_roll;
        l.sig = result.sig;
        l.ref_payout = ref_bonus;
        l.created_at = now();
    });
}

void dicegame::apply_transfer(name from, name to, asset quantity, string memo)
{
    if (from == _self || to != _self)
    {
        return;
    }

    uint64_t roll_under;
    name possible_referrer;
    string player_seed;
    uint64_t game_id;
    asset ref_bonus = asset(0, EOS_SYMBOL);

    parse_game_params(memo, &roll_under, &possible_referrer, &player_seed, &game_id);

    check_quantity(quantity);
    check_game_id(game_id);

    auto stenvironments = tenvironments.get();

    name referrer = name("");
    double fee = stenvironments.casino_fee;
    if (possible_referrer != HOUSE && possible_referrer != from && is_account(possible_referrer))
    {
        referrer = possible_referrer;
        fee = stenvironments.casino_fee - stenvironments.player_bonus;
        ref_bonus.amount = quantity.amount * stenvironments.ref_bonus / 100;
    }

    check_roll_under(roll_under);

    asset player_possible_win = calc_payout(quantity, roll_under, fee);
    eosio_assert(player_possible_win <= max_win(), "Available fund overflow");

    lock(player_possible_win);
    lock(ref_bonus);

    checksum256 player_seed_hash = sha256(const_cast<char *>(player_seed.c_str()), player_seed.size() * sizeof(char));

    auto size = transaction_size();
    char buf[size];
    read_transaction(buf, size);
    checksum256 trx_hash = sha256(buf, size);

    auto arr1 = player_seed_hash.extract_as_byte_array();
    auto arr2 = trx_hash.extract_as_byte_array();

    string mixed_hash = to_hex((char *)arr1.data(), arr1.size()) + to_hex((char *)arr2.data(), arr2.size());
    checksum256 house_seed_hash = sha256(const_cast<char *>(mixed_hash.c_str()), mixed_hash.size() * sizeof(char));

    const newBet bet{.id = available_bet_id(),
                     .game_id = game_id,
                     .player = from,
                     .amount = quantity,
                     .roll_under = roll_under,
                     .player_seed = player_seed,
                     .house_seed_hash = house_seed_hash,
                     .referrer = referrer,
                     .created_at = now()};

    tbets.emplace(_self, [&](bets &b) {
        b.id = bet.id;
        b.game_id = bet.game_id;
        b.player = bet.player;
        b.roll_under = bet.roll_under;
        b.amount = bet.amount;
        b.player_seed = bet.player_seed;
        b.house_seed_hash = bet.house_seed_hash;
        b.referrer = bet.referrer;
        b.created_at = bet.created_at;
    });

    SEND_INLINE_ACTION(*this, notify, {_self, name("active")}, {bet});
}

void dicegame::reftransfer(name to, asset quantity, string memo)
{
    action(
        permission_level{_self, name("active")},
        name("eosio.token"),
        name("transfer"),
        std::make_tuple(
            _self,
            to,
            quantity,
            memo))
        .send();

    unlock(quantity);
}

void dicegame::receipt(const resolvedBet &result)
{
    require_auth(_self);
    require_recipient(result.player);
}

void dicegame::notify(const newBet &bet)
{
    require_auth(_self);
}

void dicegame::cleanlog(uint64_t game_id)
{
    require_auth(SEVENSHELPER);
    auto entry = tlogs.find(game_id);
    tlogs.erase(entry);
}

void dicegame::reset()
{
    require_auth(SEVENSHELPER);

    auto itr = tbets.begin();
    while (itr != tbets.end())
    {
        itr = tbets.erase(itr);
    }

    auto itr2 = tlogs.begin();
    while (itr2 != tlogs.end())
    {
        itr2 = tlogs.erase(itr2);
    }

    tenvironments.remove();
}