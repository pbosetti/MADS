/*
 _____             _        __  __       _       _
|_   _|__  _ __   (_) ___  |  \/  | __ _ | |_ ___| |__
  | |/ _ \| '_ \  | |/ __| | |\/| |/ _` || __/ __| '_ \
  | | (_) | |_) | | | (__  | |  | | (_| || || (__| | | |
  |_|\___/| .__/  |_|\___| |_|  |_|\__,_| \__\___|_| |_|
          |_|

Pure MQTT-style topic wildcard matching. No dependency on Agent or ZMQ, so it
is directly unit-testable and reusable outside the subscribe path.

Grammar (mirrors MQTT):
  - '+' matches exactly one topic level.
  - '#' matches this level and everything below it, INCLUDING the level it
    replaces -- e.g. pattern "sensors/#" also matches the bare topic
    "sensors", not just "sensors/x" and deeper. Legal only as the final
    token of a pattern; a pattern where '#' appears anywhere but last is
    invalid, and topic_match() always returns false for it (never matches,
    rather than throwing -- callers get "no delivery" instead of a crash on
    a malformed sub_topic string).
  - Any other token matches its topic-level counterpart literally.
  - A pattern containing neither '+' nor '#' is an exact-match literal: it
    must equal the topic character-for-character. (Byte-prefix matching is a
    property of the raw ZeroMQ SUBSCRIBE frame, not of this function --
    literal sub_topic entries keep using that path untouched; see
    Agent::connect_sub().)

Topic levels are separated by '/'. Only a token that is *exactly* "+" or "#"
is treated as a wildcard; a token that merely contains one of those
characters (e.g. "a+b") is matched literally, mirroring strict MQTT grammar.

topic_match() alone is NOT the whole delivery rule a MADS agent applies: a
literal (wildcard-free) sub_topic entry never reaches topic_match() at
runtime, it is handed to ZeroMQ as-is and matched by byte prefix. Use
subscription_match() below whenever the question is "would this agent
actually receive this message?" -- it is the single definition both
Agent::_topic_matches_subscription() and `mads doctor --graph` are built on,
so the wire and the topology graph can never drift apart.

Author(s): Paolo Bosetti
*/

#pragma once

#include <string>
#include <string_view>

namespace Mads {

/**
 * @brief Tests whether a concrete topic matches an MQTT-style pattern.
 *
 * @param pattern Subscription pattern, e.g. "sensors/+/x" or "sensors/#".
 * @param topic Concrete topic to test, e.g. "sensors/acc/x".
 * @return true if `topic` matches `pattern`, per the grammar documented at
 * the top of this file.
 */
bool topic_match(std::string_view pattern, std::string_view topic);

/**
 * @brief Tells whether a `sub_topic` entry is a wildcard pattern.
 *
 * Deliberately as coarse as the runtime check it replaces: ANY occurrence of
 * '+' or '#' anywhere in the string, not just a whole wildcard token. A entry
 * like "a+b" therefore takes the wildcard path even though topic_match()
 * matches its "a+b" token literally -- keeping the two in step matters more
 * than the (unreachable in practice) tightening, since Agent::connect_sub()
 * and every consumer of subscription_match() must classify an entry the same
 * way or the graph and the wire disagree.
 *
 * @param sub_entry One `sub_topic` entry.
 * @return true if the entry must be handled as an MQTT-style pattern.
 */
bool has_wildcard(std::string_view sub_entry);

/// How one `sub_topic` entry matched a concrete published topic. See
/// subscription_match(): the two literal outcomes exist because a literal
/// entry is issued verbatim as a raw ZeroMQ SUBSCRIBE frame, which matches by
/// *byte prefix*, so "sensors" genuinely receives "sensors/imu/raw" too.
enum class SubMatch {
  None,     ///< Not delivered.
  Exact,    ///< Literal entry equal to the topic.
  Prefix,   ///< Literal entry that is a strict byte prefix of the topic.
  Wildcard, ///< Entry containing '+'/'#', matched per topic_match().
};

/**
 * @brief Single source of truth for "would an agent subscribing to
 * `sub_entry` receive a message published on `topic`?".
 *
 * Reproduces both stages of the runtime subscribe path exactly (see
 * Agent::connect_sub() / Agent::_topic_matches_subscription(), which are
 * implemented on top of this function):
 *
 * - A literal entry (no '+'/'#') is passed straight to
 *   `zmqpp::socket::subscribe()`, whose matching rule is a raw byte prefix.
 *   Hence `SubMatch::Prefix`: entry "sensors" receives "sensors/imu/raw", and
 *   the subscribe-all convention `sub_topic = [""]` receives everything.
 *   NOTE this is *not* topic_match()'s rule -- topic_match() is exact-literal
 *   and would reject both.
 * - A wildcard entry subscribes literal_prefix() at the ZMQ layer and is then
 *   narrowed by topic_match() before delivery, so only a full MQTT-style
 *   match counts (`SubMatch::Wildcard`).
 *
 * @param sub_entry One `sub_topic` entry (pattern or literal).
 * @param topic Concrete published topic.
 * @return How the entry matched, or SubMatch::None if it did not.
 */
SubMatch subscription_match(std::string_view sub_entry, std::string_view topic);

/**
 * @brief Convenience predicate over subscription_match().
 *
 * @param sub_entry One `sub_topic` entry.
 * @param topic Concrete published topic.
 * @return true if the message would be delivered.
 */
inline bool subscription_matches(std::string_view sub_entry,
                                 std::string_view topic) {
  return subscription_match(sub_entry, topic) != SubMatch::None;
}

/**
 * @brief Computes the longest literal (wildcard-free) prefix of a pattern.
 *
 * Meant to be issued as the raw ZeroMQ SUBSCRIBE frame for a wildcard
 * pattern: it must be a byte-prefix of *every* topic topic_match() would
 * accept for that pattern (a superset is fine -- broader than needed, e.g.
 * "sensors/+/x" -> "sensors/", and topic_match() narrows it back down on
 * the receive side -- but never a subset, or a real match gets silently
 * dropped by ZMQ before topic_match() ever runs).
 *
 * - A pattern with no '+'/'#' has no wildcard to stop at, so the whole
 *   pattern is returned unchanged.
 * - A pattern that starts with a wildcard token (e.g. "+/x") has no literal
 *   run at all, so the empty string is returned.
 * - Otherwise, the result is every full topic level before the first
 *   wildcard token. If that wildcard token is '+', a trailing '/' is
 *   appended, since '+' always requires one more level to follow (e.g.
 *   "sensors/+/x" -> "sensors/"), which keeps the ZMQ-level subscribe
 *   tighter without excluding any real match. If it is '#', NO trailing
 *   '/' is appended: '#' also matches the literal path built so far with no
 *   further level required (MQTT's "also matches the parent topic itself"
 *   rule, e.g. pattern "sensors/#" matches topic "sensors"), so
 *   "sensors/#" -> "sensors", not "sensors/" -- the latter would be a
 *   byte-prefix that excludes the bare topic "sensors" itself.
 *
 * @param pattern Subscription pattern.
 * @return The literal prefix, suitable for zmqpp::socket::subscribe().
 */
std::string literal_prefix(std::string_view pattern);

} // namespace Mads
