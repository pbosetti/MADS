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
