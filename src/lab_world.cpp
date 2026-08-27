#include "lab_world.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <cmath>

using bt::Status;

// ---------------------------------------------------------------- helpers

static float lerp_angle_f(float a, float b, float t) {
    float delta = std::fmod(b - a, 6.2831853f);
    if (delta > 3.1415926f) delta -= 6.2831853f;
    if (delta < -3.1415926f) delta += 6.2831853f;
    return a + delta * t;
}

static Ref<StandardMaterial3D> make_mat(Color c) {
    Ref<StandardMaterial3D> m;
    m.instantiate();
    m->set_albedo(c);
    return m;
}

static MeshInstance3D *add_box(Node *parent, const char *name, Vector3 pos, Vector3 size, Color c) {
    MeshInstance3D *mi = memnew(MeshInstance3D);
    Ref<BoxMesh> mesh;
    mesh.instantiate();
    mesh->set_size(size);
    mi->set_mesh(mesh);
    mi->set_material_override(make_mat(c));
    mi->set_name(name);
    parent->add_child(mi);
    mi->set_position(pos);
    return mi;
}

// capsule + dark "nose" (front is -Z) so facing direction is readable
static Node3D *add_actor(Node *parent, const char *name, float s, Color c) {
    Node3D *root = memnew(Node3D);
    root->set_name(name);
    parent->add_child(root);

    MeshInstance3D *body = memnew(MeshInstance3D);
    Ref<CapsuleMesh> cap;
    cap.instantiate(); // radius 0.5, height 2 - same as Unity's capsule
    body->set_mesh(cap);
    body->set_material_override(make_mat(c));
    root->add_child(body);

    MeshInstance3D *nose = memnew(MeshInstance3D);
    Ref<BoxMesh> nb;
    nb.instantiate();
    nb->set_size(Vector3(0.25f, 0.2f, 0.25f));
    nose->set_mesh(nb);
    nose->set_material_override(make_mat(Color(0.12f, 0.12f, 0.14f)));
    root->add_child(nose);
    nose->set_position(Vector3(0.0f, 0.25f, -0.45f));

    root->set_scale(Vector3(s, s, s));
    return root;
}

static void add_room_label(Node *parent, const char *text, Vector3 pos) {
    Label3D *l = memnew(Label3D);
    l->set_text(text);
    l->set_font_size(140);
    l->set_modulate(Color(0.35f, 0.33f, 0.30f));
    parent->add_child(l);
    l->set_position(pos + Vector3(0.0f, 0.02f, 0.0f));
    // face up, top pointing +Z: readable (not mirrored) from the south camera
    l->set_rotation_degrees(Vector3(-90.0f, 180.0f, 0.0f));
}

static Label3D *add_name_label(Node *parent, const char *text) {
    Label3D *l = memnew(Label3D);
    l->set_text(text);
    l->set_font_size(96);
    l->set_modulate(Color(0.1f, 0.1f, 0.12f));
    l->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
    parent->add_child(l);
    return l;
}

// ---------------------------------------------------------------- bt leaf nodes

namespace {

class GoTo : public bt::Node {
public:
    GoTo(LabWorld::Mover *m, std::function<Vector3()> t, float stop = 0.7f)
        : mover(m), target(std::move(t)), stopDist(stop) {}

    void reset() override { moving = false; }

protected:
    Status on_tick() override {
        Vector3 t = target();
        Vector3 flat = t - mover->body->get_position();
        flat.y = 0;

        if (flat.length() <= stopDist) {
            // stop only on the Running -> Success transition: an already
            // satisfied GoTo re-checked by its sequence must be side-effect-free
            if (moving) { mover->stop(); moving = false; }
            return Status::Success;
        }

        mover->move_to(t);
        moving = true;
        return Status::Running;
    }

    const char *type_name() const override { return "GoTo"; }

private:
    LabWorld::Mover *mover;
    std::function<Vector3()> target;
    float stopDist;
    bool moving = false;
};

// stand still and keep facing a target; the guard above decides when this ends
class Face : public bt::Node {
public:
    Face(Node3D *s, std::function<Vector3()> t) : self(s), target(std::move(t)) {}

protected:
    Status on_tick() override {
        Vector3 dir = target() - self->get_position();
        dir.y = 0;
        if (dir.length_squared() > 0.001f) {
            float yaw = std::atan2(-dir.x, -dir.z);
            self->set_rotation(Vector3(0.0f, lerp_angle_f(self->get_rotation().y, yaw, 0.3f), 0.0f));
        }
        return Status::Running;
    }

    const char *type_name() const override { return "Face"; }

private:
    Node3D *self;
    std::function<Vector3()> target;
};

class Idle : public bt::Node {
protected:
    Status on_tick() override { return Status::Running; }
    const char *type_name() const override { return "Idle"; }
};

// runs the user command queue; each command gets a freshly built subtree,
// so finished commands can never leave stale state behind
class RunCommands : public bt::Node {
public:
    explicit RunCommands(LabWorld *w) : world(w) {}

    void reset() override { current.reset(); }

protected:
    Status on_tick() override {
        if (!current) {
            if (world->roboQueue.empty()) return Status::Failure;
            current.reset(world->build_robo_command(world->roboQueue.front()));
        }
        Status s = current->tick();
        if (s != Status::Running) {
            world->roboQueue.pop_front();
            current.reset();
        }
        return Status::Running;
    }

    const char *type_name() const override { return "Commands"; }

private:
    LabWorld *world;
    std::unique_ptr<bt::Node> current;
};

} // namespace

// ---------------------------------------------------------------- Mover

void LabWorld::Mover::move_to(const Vector3 &target) {
    // re-asserting the same destination is free; a moving target only
    // triggers a re-path after drifting half a meter
    if (hasDest && (dest - target).length_squared() < 0.25f) return;
    dest = target;
    hasDest = true;
    path = world->find_path(body->get_position(), target);
    index = 0;
}

void LabWorld::Mover::stop() {
    hasDest = false;
    path.clear();
    index = 0;
}

void LabWorld::Mover::update(float dt) {
    if (arrived()) return;

    Vector3 wp = path[index];
    wp.y = body->get_position().y;
    Vector3 to = wp - body->get_position();

    if (to.length() < 0.35f) {
        index++;
        return;
    }

    Vector3 dir = to.normalized();
    body->set_position(body->get_position() + dir * speed * dt);
    float yaw = std::atan2(-dir.x, -dir.z);
    body->set_rotation(Vector3(0.0f, lerp_angle_f(body->get_rotation().y, yaw, 10.f * dt), 0.0f));
}

// ---------------------------------------------------------------- graph

void LabWorld::build_graph() {
    // NOTE: X is mirrored relative to the Unity version - Godot is
    // right-handed, so this makes the on-screen layout match (kitchen left)
    cps = {
        Vector3(0.0f, 0.0f, -2.5f),   // 0 LivingCenter
        Vector3(5.0f, 0.0f, -4.6f),   // 1 LivingWest
        Vector3(0.0f, 0.0f, -5.2f),   // 2 DoorInside
        Vector3(0.0f, 0.0f, -7.2f),   // 3 Porch
        Vector3(4.0f, 0.0f, 1.0f),    // 4 KitchenDoor
        Vector3(4.0f, 0.0f, 3.8f),    // 5 Kitchen
        Vector3(6.2f, 0.0f, 4.2f),    // 6 Fridge
        Vector3(-4.0f, 0.0f, 1.0f),   // 7 GuestDoor
        Vector3(-4.0f, 0.0f, 3.8f),   // 8 GuestRoom
        Vector3(-5.3f, 0.0f, 3.4f),   // 9 Bed
    };

    adj.assign(cps.size(), {});
    auto edge = [this](int a, int b) {
        adj[a].push_back(b);
        adj[b].push_back(a);
    };
    edge(0, 1); edge(0, 2); edge(0, 4); edge(0, 7);
    edge(1, 4); edge(2, 3); edge(4, 5); edge(5, 6);
    edge(7, 8); edge(8, 9);
}

int LabWorld::nearest_cp(const Vector3 &p) const {
    int best = -1;
    float bestDist = 1e30f;
    for (int i = 0; i < (int)cps.size(); i++) {
        float d = (cps[i] - p).length_squared();
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// BFS over the checkpoint graph, then the exact target as the last waypoint
std::vector<Vector3> LabWorld::find_path(const Vector3 &from, const Vector3 &to) const {
    std::vector<Vector3> path;
    int start = nearest_cp(from);
    int goal = nearest_cp(to);

    if (start >= 0 && goal >= 0 && start != goal) {
        std::vector<int> parent(cps.size(), -2);
        std::deque<int> queue;
        parent[start] = -1;
        queue.push_back(start);

        while (!queue.empty()) {
            int cp = queue.front();
            queue.pop_front();
            if (cp == goal) break;
            for (int n : adj[cp]) {
                if (parent[n] != -2) continue;
                parent[n] = cp;
                queue.push_back(n);
            }
        }

        if (parent[goal] != -2) {
            for (int cp = goal; cp != -1; cp = parent[cp])
                path.insert(path.begin(), cps[cp]);
        }
    }
    // start == goal: same zone, walk straight to the target

    path.push_back(to);

    // don't walk BACK to our zone's node when we're already closer to the
    // next waypoint than that node is (kills path-flapping on moving targets)
    if (path.size() >= 2 && (path[1] - from).length_squared() <= (path[1] - path[0]).length_squared())
        path.erase(path.begin());

    return path;
}

// ---------------------------------------------------------------- scene

void LabWorld::build_scene() {
    // floors
    add_box(this, "Floor", Vector3(0.0f, -0.05f, 0.0f), Vector3(16.0f, 0.1f, 12.0f), Color(0.85f, 0.82f, 0.76f));
    add_box(this, "Porch", Vector3(0.0f, -0.05f, -7.4f), Vector3(6.0f, 0.1f, 2.8f), Color(0.45f, 0.45f, 0.48f));

    // walls (south wall has the entrance gap at x in [-0.7, 0.7])
    Color wall(0.93f, 0.93f, 0.90f);
    const float H = 2.5f, T = 0.2f;
    add_box(this, "Wall_N", Vector3(0.0f, H / 2, 6.0f), Vector3(16.4f, H, T), wall);
    add_box(this, "Wall_S_West", Vector3(-4.4f, H / 2, -6.0f), Vector3(7.4f, H, T), wall);
    add_box(this, "Wall_S_East", Vector3(4.4f, H / 2, -6.0f), Vector3(7.4f, H, T), wall);
    add_box(this, "Wall_W", Vector3(-8.0f, H / 2, 0.0f), Vector3(T, H, 12.4f), wall);
    add_box(this, "Wall_E", Vector3(8.0f, H / 2, 0.0f), Vector3(T, H, 12.4f), wall);
    add_box(this, "Wall_I_West", Vector3(-6.35f, H / 2, 1.0f), Vector3(3.3f, H, T), wall);
    add_box(this, "Wall_I_Mid", Vector3(0.0f, H / 2, 1.0f), Vector3(6.6f, H, T), wall);
    add_box(this, "Wall_I_East", Vector3(6.35f, H / 2, 1.0f), Vector3(3.3f, H, T), wall);
    add_box(this, "Wall_KitchenGuest", Vector3(0.0f, H / 2, 3.5f), Vector3(T, H, 5.0f), wall);

    // room labels
    add_room_label(this, "KITCHEN", Vector3(4.0f, 0.0f, 3.5f));
    add_room_label(this, "GUEST ROOM", Vector3(-3.5f, 0.0f, 3.0f));
    add_room_label(this, "LIVING ROOM", Vector3(0.0f, 0.0f, -4.0f));
    add_room_label(this, "ENTRANCE", Vector3(0.0f, 0.0f, -7.9f));

    // props
    Color prop(0.55f, 0.50f, 0.65f);
    add_box(this, "KitchenCounter", Vector3(4.0f, 0.45f, 5.55f), Vector3(4.0f, 0.9f, 0.6f), prop);
    add_box(this, "GuestBed", Vector3(-6.5f, 0.25f, 4.5f), Vector3(2.0f, 0.5f, 1.4f), prop);
    add_box(this, "Fridge", Vector3(7.3f, 1.0f, 5.3f), Vector3(1.0f, 2.0f, 0.9f), Color(0.92f, 0.94f, 0.96f));

    // entrance door + bell
    door = add_box(this, "EntranceDoor", Vector3(0.0f, 1.1f, -6.0f), Vector3(1.36f, 2.2f, 0.12f), Color(0.55f, 0.36f, 0.20f));
    doorClosedPos = door->get_position();

    MeshInstance3D *bell = memnew(MeshInstance3D);
    Ref<SphereMesh> sph;
    sph.instantiate();
    sph->set_radius(0.08f);
    sph->set_height(0.16f);
    bell->set_mesh(sph);
    bell->set_material_override(make_mat(Color(0.9f, 0.15f, 0.15f)));
    bell->set_name("Doorbell");
    add_child(bell);
    bell->set_position(Vector3(-1.05f, 1.35f, -6.16f));

    // the cup, on the kitchen counter
    MeshInstance3D *cupMesh = memnew(MeshInstance3D);
    Ref<CylinderMesh> cyl;
    cyl.instantiate();
    cyl->set_top_radius(0.125f);
    cyl->set_bottom_radius(0.125f);
    cyl->set_height(0.24f);
    cupMesh->set_mesh(cyl);
    cupMesh->set_material_override(make_mat(Color(0.95f, 0.85f, 0.20f)));
    cupMesh->set_name("Cup");
    add_child(cupMesh);
    cupMesh->set_position(Vector3(3.6f, 1.02f, 5.5f));
    cup = cupMesh;

    // characters
    robo = add_actor(this, "Robo", 1.0f, Color(0.25f, 0.55f, 0.95f));
    robo->set_position(Vector3(0.0f, 1.0f, -2.0f));
    kid = add_actor(this, "Kid", 0.55f, Color(1.0f, 0.6f, 0.15f));
    kid->set_position(Vector3(-4.0f, 0.55f, 3.5f));
    guest = add_actor(this, "Guest", 0.9f, Color(0.2f, 0.7f, 0.4f));
    guest->set_position(Vector3(-0.9f, 0.9f, -7.4f));
    resident = add_actor(this, "Resident", 0.9f, Color(0.15f, 0.6f, 0.65f));
    resident->set_position(Vector3(3.4f, 0.9f, -3.8f));

    nameLabels[0] = add_name_label(this, "Robo");
    nameLabels[1] = add_name_label(this, "Kid");
    nameLabels[2] = add_name_label(this, "Guest");
    nameLabels[3] = add_name_label(this, "Resident");

    // camera + light
    cam = memnew(Camera3D);
    add_child(cam);
    cam->set_position(Vector3(0.0f, 16.0f, -12.0f));
    cam->look_at(Vector3(0.0f, 0.0f, -0.5f));

    DirectionalLight3D *sun = memnew(DirectionalLight3D);
    add_child(sun);
    sun->set_rotation_degrees(Vector3(-50.0f, -30.0f, 0.0f));
    sun->set_shadow(true);

    // ambient light + background so shadows aren't pitch black
    Ref<Environment> env;
    env.instantiate();
    env->set_background(Environment::BG_COLOR);
    env->set_bg_color(Color(0.32f, 0.33f, 0.36f));
    env->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
    env->set_ambient_light_color(Color(1.0f, 1.0f, 1.0f));
    env->set_ambient_light_energy(0.7f);
    WorldEnvironment *we = memnew(WorldEnvironment);
    we->set_environment(env);
    add_child(we);

    roboMover = { robo, this };
    kidMover = { kid, this };
    guestMover = { guest, this };
}

// ---------------------------------------------------------------- brains

void LabWorld::set_door_open(bool open) {
    door->set_position(open ? doorClosedPos + Vector3(-1.25f, 0.0f, 0.0f) : doorClosedPos);
}

// hysteresis: freeze under 2 m, resume only past 2.5 m - no border flapping
bool LabWorld::kid_near() {
    Vector3 d = kid->get_position() - robo->get_position();
    d.y = 0;
    kidHold = d.length() < (kidHold ? 2.5f : 2.0f);
    return kidHold;
}

bt::Node *LabWorld::build_robo_command(int cmd) {
    using namespace bt;

    switch (cmd) {
        case ROBO_BRING_CUP:
            return L(new Sequence({
                L(new GoTo(&roboMover, [this] { return cup->get_global_position(); }, 1.0f), "GoTo(cup)"),
                L(new Do([this] {
                    cup->get_parent()->remove_child(cup);
                    robo->add_child(cup);
                    cup->set_position(Vector3(0.0f, 0.25f, -0.55f));
                    return Status::Success;
                }), "PickUp"),
                L(new GoTo(&roboMover, [this] { return resident->get_position(); }, 1.1f), "GoTo(Resident)"),
                L(new Do([this] {
                    cup->get_parent()->remove_child(cup);
                    add_child(cup);
                    Vector3 fwd = -robo->get_global_transform().get_basis().get_column(2);
                    Vector3 p = robo->get_position() + fwd * 0.6f;
                    p.y = 0.12f;
                    cup->set_position(p);
                    return Status::Success;
                }), "GiveCup"),
            }), "BringCup");

        case ROBO_COME_HERE:
            return L(new GoTo(&roboMover, [this] { return resident->get_position(); }, 1.1f), "ComeHere");
    }
    return L(new Idle(), "Unknown");
}

void LabWorld::build_brains() {
    using namespace bt;

    roboRoot.reset(L(new Selector({
        // top priority: a kid nearby preempts everything - stop, watch, wait
        L(new Sequence({
            L(new Condition([this] { return kid_near(); }), "KidNear?"),
            L(new Do([this] { roboMover.stop(); return Status::Success; }), "Stop"),
            L(new Face(robo, [this] { return kid->get_position(); }), "Watch(kid)"),
        }), "KidSafety"),

        L(new AutoReset(new Sequence({
            L(new Condition([this] { return doorbellPending; }), "Doorbell?"),
            L(new GoTo(&roboMover, [] { return Vector3(0.0f, 0.0f, -4.6f); }), "GoTo(door)"),
            L(new Do([this] { set_door_open(true); return Status::Success; }), "OpenDoor"),
            L(new Wait(2.0), "Greet"),
            L(new Do([this] {
                set_door_open(false);
                doorbellPending = false;
                return Status::Success;
            }), "CloseDoor"),
        })), "AnswerDoor"),

        L(new RunCommands(this), "Commands"),

        L(new Idle(), "Idle"),
    }), "Robo"));

    Vector3 kidHome = kid->get_position();
    Vector3 bedSpot(-5.4f, 0.0f, 4.2f);

    kidRoot.reset(L(new Selector({
        L(new AutoReset(new Sequence({
            L(new Condition([this] { return kidCmd == KID_CHECK_ROBO; }), "CheckRobo?"),
            L(new GoTo(&kidMover, [this] { return robo->get_position(); }, 1.4f), "GoTo(Robo)"),
            L(new Wait(2.0), "Watch"),
            // going home is part of the command, or the kid parks next to
            // Robo forever and Robo's safety branch never releases
            L(new GoTo(&kidMover, [kidHome] { return kidHome; }), "GoHome"),
            L(new Do([this] { kidCmd = KID_NONE; return Status::Success; }), "Done"),
        })), "CheckRobo"),

        L(new AutoReset(new Sequence({
            L(new Condition([this] { return kidCmd == KID_GO_BED; }), "GoToBed?"),
            L(new GoTo(&kidMover, [bedSpot] { return bedSpot; }), "GoTo(bed)"),
            L(new Do([this] { kidCmd = KID_NONE; return Status::Success; }), "Done"),
        })), "GoToBed"),

        L(new Idle(), "Idle"),
    }), "Kid"));

    Vector3 bellSpot(-0.9f, 0.0f, -6.9f);
    Vector3 wanderSpot(-2.4f, 0.0f, -8.1f);

    guestRoot.reset(L(new Selector({
        L(new AutoReset(new Sequence({
            L(new Condition([this] { return guestCmd == GUEST_RING; }), "RingBell?"),
            L(new GoTo(&guestMover, [bellSpot] { return bellSpot; }, 0.4f), "GoTo(bell)"),
            L(new Wait(0.5), "Press"),
            L(new Do([this] {
                doorbellPending = true;
                guestCmd = GUEST_NONE;
                return Status::Success;
            }), "Ring"),
        })), "RingBell"),

        L(new AutoReset(new Sequence({
            L(new Condition([this] { return guestCmd == GUEST_WANDER; }), "Wander?"),
            L(new GoTo(&guestMover, [wanderSpot] { return wanderSpot; }, 0.4f), "GoTo(away)"),
            L(new Do([this] { guestCmd = GUEST_NONE; return Status::Success; }), "Done"),
        })), "WanderOff"),

        L(new Idle(), "Idle"),
    }), "Guest"));
}

// ---------------------------------------------------------------- UI

void LabWorld::build_ui() {
    ColorRect *bg = memnew(ColorRect);
    bg->set_color(Color(0.0f, 0.0f, 0.0f, 0.35f));
    bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    add_child(bg);
    bg->set_position(Vector2(8.0f, 8.0f));
    bg->set_size(Vector2(620.0f, 520.0f));

    console = memnew(RichTextLabel);
    console->set_use_bbcode(true);
    console->set_scroll_follow(true);
    console->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    console->add_theme_font_size_override("normal_font_size", 24);
    add_child(console);
    console->set_position(Vector2(16.0f, 12.0f));
    console->set_size(Vector2(604.0f, 512.0f));
}

static const char *status_color(Status s) {
    if (s == Status::Running) return "#e2c04c";
    if (s == Status::Success) return "#63d063";
    return "#8a8a8a";
}

static const char *status_name(Status s) {
    if (s == Status::Running) return "Running";
    if (s == Status::Success) return "Success";
    return "Failure";
}

void LabWorld::update_console() {
    if (bt::Trace::last.empty()) return;

    std::string sig;
    String block;
    for (const auto &e : bt::Trace::last) {
        sig += std::to_string(e.depth) + e.name + status_name(e.status) + "\n";
        String indent;
        for (int i = 0; i < e.depth; i++) indent += "   ";
        block += indent + String(e.name.c_str()) +
                 " [color=" + status_color(e.status) + "]" + status_name(e.status) + "[/color]\n";
    }
    if (sig == lastSig) return;
    lastSig = sig;

    if (consoleLines > 300) {
        console->clear();
        consoleLines = 0;
    }
    console->append_text("[color=#6ab0f9][" + String::num(clockNow, 1) + "s][/color]\n" + block);
    consoleLines += (int)bt::Trace::last.size() + 1;
}

Node3D *LabWorld::menu_actor_node(int actor) const {
    return actor == 0 ? resident : actor == 1 ? kid : guest;
}

static const char *menu_labels[3][2] = {
    { "Bring me the cup", "Robo, come here" },
    { "Go check what Robo is doing", "Go to bed" },
    { "Go ring the doorbell", "Wander off" },
};
static const char *actor_names[3] = { "Resident", "Kid", "Guest" };

void LabWorld::open_menu(int actor) {
    close_menu();

    ColorRect *panel = memnew(ColorRect);
    panel->set_color(Color(0.1f, 0.1f, 0.14f, 0.9f));
    add_child(panel);
    Vector2 sp = cam->unproject_position(menu_actor_node(actor)->get_global_position());
    panel->set_position(sp + Vector2(26.0f, -30.0f));
    panel->set_size(Vector2(430.0f, 46.0f + 2 * 58.0f));

    Label *head = memnew(Label);
    head->set_text(actor_names[actor]);
    head->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    head->add_theme_font_size_override("font_size", 24);
    panel->add_child(head);
    head->set_position(Vector2(0.0f, 8.0f));
    head->set_size(Vector2(430.0f, 30.0f));

    for (int i = 0; i < 2; i++) {
        Button *b = memnew(Button);
        b->set_text(menu_labels[actor][i]);
        b->add_theme_font_size_override("font_size", 22);
        panel->add_child(b);
        b->set_position(Vector2(8.0f, 42.0f + i * 58.0f));
        b->set_size(Vector2(414.0f, 52.0f));
        b->connect("pressed", callable_mp(this, &LabWorld::menu_pick).bind(actor, i));
    }

    menuRoot = panel;
}

void LabWorld::close_menu() {
    if (menuRoot != nullptr) {
        menuRoot->queue_free();
        menuRoot = nullptr;
    }
}

void LabWorld::menu_pick(int actor, int item) {
    if (actor == 0) roboQueue.push_back(item == 0 ? ROBO_BRING_CUP : ROBO_COME_HERE);
    if (actor == 1) kidCmd = item == 0 ? KID_CHECK_ROBO : KID_GO_BED;
    if (actor == 2) guestCmd = item == 0 ? GUEST_RING : GUEST_WANDER;
    close_menu();
}

// ---------------------------------------------------------------- lifecycle

void LabWorld::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;

    build_graph();
    build_scene();
    build_brains();
    build_ui();
}

void LabWorld::_process(double delta) {
    if (Engine::get_singleton()->is_editor_hint()) return;
    if (roboRoot == nullptr) return;

    clockNow += delta;
    bt::Clock::now = clockNow;

    // decision layer: 10 Hz; only Robo's tree is traced for the console
    tickTimer += delta;
    while (tickTimer >= 0.1) {
        tickTimer -= 0.1;
        bt::Trace::begin();
        roboRoot->tick();
        bt::Trace::end();
        kidRoot->tick();
        guestRoot->tick();
    }

    // actuation layer: every frame
    float dt = (float)delta;
    roboMover.update(dt);
    kidMover.update(dt);
    guestMover.update(dt);

    // name labels follow their actors
    Node3D *actors[4] = { robo, kid, guest, resident };
    float scales[4] = { 1.0f, 0.55f, 0.9f, 0.9f };
    for (int i = 0; i < 4; i++)
        nameLabels[i]->set_position(actors[i]->get_position() + Vector3(0.0f, scales[i] + 0.5f, 0.0f));

    update_console();
}

void LabWorld::_unhandled_input(const Ref<InputEvent> &event) {
    if (Engine::get_singleton()->is_editor_hint()) return;

    auto *mb = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (mb == nullptr || !mb->is_pressed() || mb->get_button_index() != MOUSE_BUTTON_LEFT) return;

    // button clicks are consumed by the UI and never reach here,
    // so any click landing here closes the menu and tries to pick an actor
    close_menu();

    Vector2 mp = mb->get_position();
    int best = -1;
    float bestDist = 70.0f;
    for (int i = 0; i < 3; i++) {
        Vector2 sp = cam->unproject_position(menu_actor_node(i)->get_global_position());
        float d = sp.distance_to(mp);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best >= 0) open_menu(best);
}
