// The whole lab in one GDExtension class: builds the apartment scene in _ready,
// ticks three behavior trees at 10 Hz, moves characters every frame,
// draws the tree console and the click menus.
#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "bt.h"

using namespace godot;

class LabWorld : public Node3D {
    GDCLASS(LabWorld, Node3D)

public:
    void _ready() override;
    void _process(double delta) override;
    void _unhandled_input(const Ref<InputEvent> &event) override;

    void menu_pick(int actor, int item);

    // command ids
    enum { ROBO_BRING_CUP, ROBO_COME_HERE };
    enum { KID_NONE, KID_CHECK_ROBO, KID_GO_BED };
    enum { GUEST_NONE, GUEST_RING, GUEST_WANDER };

    // ---- blackboard (shared world state)
    bool doorbellPending = false;
    std::deque<int> roboQueue;
    int kidCmd = KID_NONE;
    int guestCmd = GUEST_NONE;

    // ---- actuation layer: walks a body along checkpoint paths every frame
    struct Mover {
        Node3D *body = nullptr;
        LabWorld *world = nullptr;
        std::vector<Vector3> path;
        size_t index = 0;
        bool hasDest = false;
        Vector3 dest;
        float speed = 2.5f;

        void move_to(const Vector3 &target);
        void stop();
        bool arrived() const { return !hasDest || index >= path.size(); }
        void update(float dt);
    };

    Mover roboMover, kidMover, guestMover;

    Node3D *robo = nullptr, *kid = nullptr, *guest = nullptr, *resident = nullptr;
    Node3D *door = nullptr, *cup = nullptr;

    std::vector<Vector3> find_path(const Vector3 &from, const Vector3 &to) const;
    bt::Node *build_robo_command(int cmd);
    void set_door_open(bool open);
    bool kid_near();

protected:
    static void _bind_methods() {}

private:
    Camera3D *cam = nullptr;
    Vector3 doorClosedPos;
    bool kidHold = false;

    // checkpoint graph
    std::vector<Vector3> cps;
    std::vector<std::vector<int>> adj;
    int nearest_cp(const Vector3 &p) const;

    // brains
    std::unique_ptr<bt::Node> roboRoot, kidRoot, guestRoot;
    double tickTimer = 0.0;
    double clockNow = 0.0;

    // UI
    RichTextLabel *console = nullptr;
    std::string lastSig;
    int consoleLines = 0;
    Control *menuRoot = nullptr;
    Label3D *nameLabels[4] = {};

    void build_scene();
    void build_graph();
    void build_brains();
    void build_ui();
    void update_console();
    void open_menu(int actor);
    void close_menu();

    Node3D *menu_actor_node(int actor) const;
};
