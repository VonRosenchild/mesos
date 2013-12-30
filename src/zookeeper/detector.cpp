#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include "process/logging.hpp"
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"

using namespace process;

using std::set;
using std::string;

namespace zookeeper {

class LeaderDetectorProcess : public Process<LeaderDetectorProcess>
{
public:
  LeaderDetectorProcess(Group* group);
  virtual ~LeaderDetectorProcess();
  virtual void initialize();

  // LeaderDetector implementation.
  Future<Option<Group::Membership> > detect(
      const Option<Group::Membership>& previous);

private:
  // Helper that sets up the watch on the group.
  void watch(const set<Group::Membership>& expected);

  // Invoked when the group memberships have changed.
  void watched(Future<set<Group::Membership> > memberships);

  Group* group;
  Option<Group::Membership> leader;
  set<Promise<Option<Group::Membership> >*> promises;
};


LeaderDetectorProcess::LeaderDetectorProcess(Group* _group)
  : group(_group), leader(None()) {}


LeaderDetectorProcess::~LeaderDetectorProcess()
{
  foreach (Promise<Option<Group::Membership> >* promise, promises) {
    promise->future().discard();
    delete promise;
  }
  promises.clear();
}


void LeaderDetectorProcess::initialize()
{
  watch(set<Group::Membership>());
}


Future<Option<Group::Membership> > LeaderDetectorProcess::detect(
    const Option<Group::Membership>& previous)
{
  // Return immediately if the incumbent leader is different from the
  // expected.
  if (leader != previous) {
    return leader;
  }

  // Otherwise wait for the next election result.
  Promise<Option<Group::Membership> >* promise =
    new Promise<Option<Group::Membership> >();
  promises.insert(promise);
  return promise->future();
}


void LeaderDetectorProcess::watch(const set<Group::Membership>& expected)
{
  group->watch(expected)
    .onAny(defer(self(), &Self::watched, lambda::_1));
}


void LeaderDetectorProcess::watched(Future<set<Group::Membership> > memberships)
{
  CHECK(!memberships.isDiscarded());

  if (memberships.isFailed()) {
    LOG(ERROR) << "Failed to watch memberships: " << memberships.failure();
    leader = None();
    foreach (Promise<Option<Group::Membership> >* promise, promises) {
      promise->fail(memberships.failure());
      delete promise;
    }
    promises.clear();
    return;
  }

  // Update leader status based on memberships.
  if (leader.isSome() && memberships.get().count(leader.get()) == 0) {
    VLOG(1) << "The current leader (id=" << leader.get().id() << ") is lost";
  }

  // Run an "election". The leader is the oldest member (smallest
  // membership id). We do not fulfill any of our promises if the
  // incumbent wins the election.
  Option<Group::Membership> current;
  foreach (const Group::Membership& membership, memberships.get()) {
    current = min(current, membership);
  }

  if (current != leader) {
    LOG(INFO) << "Detected a new leader: "
              << (current.isSome()
                  ? "'(id='" + stringify(current.get().id()) + "')"
                  : "None");

    foreach (Promise<Option<Group::Membership> >* promise, promises) {
      promise->set(current);
      delete promise;
    }
    promises.clear();
  }

  leader = current;
  watch(memberships.get());
}


LeaderDetector::LeaderDetector(Group* group)
{
  process = new LeaderDetectorProcess(group);
  spawn(process);
}


LeaderDetector::~LeaderDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Option<Group::Membership> > LeaderDetector::detect(
    const Option<Group::Membership>& membership)
{
  return dispatch(process, &LeaderDetectorProcess::detect, membership);
}

} // namespace zookeeper {