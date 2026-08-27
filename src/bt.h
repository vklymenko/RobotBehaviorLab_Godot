// Behavior tree core - pure C++, no engine dependencies.
// Reset() so finished/abandoned branches can run again.
#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace bt {

enum class Status { Success, Failure, Running };

// shared clock for Wait nodes; the world updates it every frame
struct Clock {
    static inline double now = 0.0;
};

class Node;

// records the visited nodes of the last traced tick (read by the console)
class Trace {
public:
    struct Entry {
        int depth;
        std::string name;
        Status status;
    };

    static inline std::vector<Entry> last;

    static inline std::vector<Entry> current;
    static inline std::vector<size_t> stack;
    static inline int depth = 0;
    static inline bool active = false;

    static void begin() { active = true; current.clear(); stack.clear(); depth = 0; }
    static void end() { active = false; last = current; }

    static void enter(const std::string &name) {
        if (!active) return;
        stack.push_back(current.size());
        current.push_back({depth, name, Status::Running});
        depth++;
    }

    static void exit(Status s) {
        if (!active) return;
        depth--;
        current[stack.back()].status = s;
        stack.pop_back();
    }
};

class Node {
public:
    std::string label;

    virtual ~Node() = default;

    std::string display_name() const { return label.empty() ? type_name() : label; }

    // wrapper: every tick is recorded so the debug console can show the tree
    Status tick() {
        Trace::enter(display_name());
        Status s = on_tick();
        Trace::exit(s);
        return s;
    }

    // clear per-run state so a finished or abandoned branch can run again
    virtual void reset() {}

protected:
    virtual Status on_tick() = 0;
    virtual const char *type_name() const { return "Node"; }
};

// helper: set a label at construction site - L(new GoTo(...), "GoTo(cup)")
inline Node *L(Node *n, const char *label) {
    n->label = label;
    return n;
}

// "Do A, then B, then C." Stops on the first child that isn't Success.
class Sequence : public Node {
public:
    Sequence(std::initializer_list<Node *> nodes) {
        for (Node *n : nodes) children.emplace_back(n);
    }

    void reset() override {
        for (auto &c : children) c->reset();
    }

protected:
    Status on_tick() override {
        for (auto &c : children) {
            Status s = c->tick();
            if (s != Status::Success) return s;
        }
        return Status::Success;
    }

    const char *type_name() const override { return "Sequence"; }

private:
    std::vector<std::unique_ptr<Node>> children;
};

// "Try A, else B, else C." Priorities live here: child order = importance.
class Selector : public Node {
public:
    Selector(std::initializer_list<Node *> nodes) {
        for (Node *n : nodes) children.emplace_back(n);
    }

    void reset() override {
        for (auto &c : children) c->reset();
    }

protected:
    Status on_tick() override {
        for (auto &c : children) {
            Status s = c->tick();
            if (s != Status::Failure) return s;
        }
        return Status::Failure;
    }

    const char *type_name() const override { return "Selector"; }

private:
    std::vector<std::unique_ptr<Node>> children;
};

class Condition : public Node {
public:
    explicit Condition(std::function<bool()> f) : check(std::move(f)) {}

protected:
    Status on_tick() override { return check() ? Status::Success : Status::Failure; }
    const char *type_name() const override { return "Condition"; }

private:
    std::function<bool()> check;
};

// one-shot action as a lambda
class Do : public Node {
public:
    explicit Do(std::function<Status()> f) : act(std::move(f)) {}

protected:
    Status on_tick() override { return act(); }
    const char *type_name() const override { return "Do"; }

private:
    std::function<Status()> act;
};

// Waits N seconds of Clock time. Starts counting on the FIRST tick.
class Wait : public Node {
public:
    explicit Wait(double seconds) : duration(seconds) {}

    void reset() override { start = -1.0; }

protected:
    Status on_tick() override {
        if (start < 0.0) start = Clock::now;
        return Clock::now >= start + duration ? Status::Success : Status::Running;
    }

    const char *type_name() const override { return "Wait"; }

private:
    double duration;
    double start = -1.0;
};

// Resets its child whenever the child finishes (Success or Failure),
// so branches like "answer the door" can run again with fresh state.
class AutoReset : public Node {
public:
    explicit AutoReset(Node *n) : child(n) {}

    void reset() override { child->reset(); }

protected:
    Status on_tick() override {
        Status s = child->tick();
        if (s != Status::Running) child->reset();
        return s;
    }

    const char *type_name() const override { return "AutoReset"; }

private:
    std::unique_ptr<Node> child;
};

} // namespace bt
