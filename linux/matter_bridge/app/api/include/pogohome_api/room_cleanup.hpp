// What Power Base management and the shared settings say to each other about
// the room clean-up of a Power Base that left this hub's list
// (docs/power-base-management.md, sections 9 and 14). Neither includes the
// other: management decides WHOSE room assignment is owed a clean-up and
// whether it still is, the settings store reads and writes the assignments.
//
// A clean-up is about one registration's assignments, never about "whatever
// room this module has when the clean-up finally runs":
//   - which assignment: the room the module was in when its Base left the
//     list (RoomCleanupTarget::room_id, read at that moment). A room it was
//     given since is another assignment and is not touched;
//   - whose: decided by management IN THE SAME SECTION as the write
//     (RoomCleanupGuard::commit()) -- a module that is listed under a Power
//     Base by then (it moved, or its Base was registered again), or that was
//     listed under one at any time since the clean-up was noted, is not this
//     clean-up's any more, however long ago the clean-up was noted. In that
//     section no report can list a module under a Power Base, so nothing
//     falls between the decision and the write: a module that moves keeps the
//     room it has, whether or not anyone saved the settings in between.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace pogohome_api {

// One room assignment a clean-up is to clear.
struct RoomCleanupTarget {
    std::string module_uuid; // 16 lowercase hex
    // The room the module was in when its Power Base left this hub's list
    // (canonical id), when the settings could be read at that moment. Known:
    // only an assignment to exactly this room is cleared. Not known (the
    // settings could not be read then, or the clean-up was stored by a hub
    // that did not write it down): whatever room the module has.
    bool room_known = false;
    std::string room_id;
};

// What ties the settings write of a clean-up to the list of Power Bases.
//
//   the section    guard.commit(modules, write): decides, for the modules one
//                  attempt is about to clear, which are still this clean-up's
//                  and, without letting go of what it decided from, calls
//                  `write` with those (not at all when none is). While
//                  `write` runs no report is applied to the Registry, so
//                  "listed under no Power Base" is true of every module
//                  `write` is given until `write` has returned. This is what
//                  management hands over
//                  (PowerBaseManagement::ProcessSettingsCleanups()), and what
//                  the write must go through: any answer given BEFORE the
//                  write leaves a gap between the answer and the write.
//   the question   guard(module): a plain yes or no, for a caller that has no
//                  Registry to hold still (the settings side's own tests).
//                  Such a guard has no section: commit() asks the question
//                  and writes -- unbound. Management's guard has no question
//                  (every module is a candidate until the section decides).
//
// Unset: every target is the clean-up's.
class RoomCleanupGuard {
public:
    using Question = std::function<bool(const std::string &module_uuid)>;
    // The one write of an attempt, for the modules (16 lowercase hex) that
    // are still the clean-up's. Never called with an empty list.
    using Write = std::function<void(const std::vector<std::string> &owed)>;
    using Section = std::function<void(const std::vector<std::string> &modules, const Write &write)>;

    RoomCleanupGuard() = default;
    RoomCleanupGuard(std::nullptr_t) {}
    template <typename F, typename = std::enable_if_t<!std::is_same<std::decay_t<F>, RoomCleanupGuard>::value &&
                                                      std::is_invocable_r<bool, F &, const std::string &>::value>>
    RoomCleanupGuard(F question) : question_(std::move(question))
    {
    }
    RoomCleanupGuard(Question question, Section section) : question_(std::move(question)), section_(std::move(section)) {}

    explicit operator bool() const { return static_cast<bool>(question_) || static_cast<bool>(section_); }

    bool operator()(const std::string &module_uuid) const { return !question_ || question_(module_uuid); }

    void commit(const std::vector<std::string> &modules, const Write &write) const
    {
        if (section_) {
            section_(modules, write);
            return;
        }
        std::vector<std::string> owed;
        for (const std::string &module : modules) {
            if ((*this)(module)) {
                owed.push_back(module);
            }
        }
        if (!owed.empty() && write) {
            write(owed);
        }
    }

private:
    Question question_;
    Section section_;
};

// Clears the room assignments `targets` name, each only while `still_owed`
// says so -- through still_owed.commit(), see above. True when that is done
// or there was nothing to do; false when it could not be done now (the
// caller retries). Must be idempotent.
using RoomUnassigner =
    std::function<bool(const std::vector<RoomCleanupTarget> &targets, const RoomCleanupGuard &still_owed)>;

// The room each of these modules (16 lowercase hex) is in right now: one
// entry, with `room_known`, per module that HAS a room; a module without one
// gets no entry. False when that cannot be told now (the settings file is
// damaged): `out` is then meaningless. Must not block -- it is called with
// Power Base management's lock held.
using RoomReader = std::function<bool(const std::vector<std::string> &module_uuids, std::vector<RoomCleanupTarget> &out)>;

} // namespace pogohome_api
