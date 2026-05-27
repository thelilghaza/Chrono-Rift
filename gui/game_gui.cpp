#include "../common/game_shared.h"

#include <SFML/Graphics.hpp>
#include <X11/Xlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr int kWindowW = 1280;
constexpr int kWindowH = 760;
constexpr float kPi = 3.14159265f;

struct Snapshot {
    int game_running = 0;
    int acting_id = -1;
    int acting_side = 0;
    int current_turn = 0;
    int roll_number_seed = 0;
    int setup_phase = 1;
    int setup_party_size = 1;
    int players_alive = 0;
    int enemies_alive = 0;
    int total_enemy_kills = 0;
    int player_count = 0;
    int enemy_count = 0;
    std::array<int, kMaxPlayers> php{};
    std::array<int, kMaxPlayers> pmax{};
    std::array<int, kMaxPlayers> pstam{};
    std::array<int, kMaxPlayers> pmaxstam{};
    std::array<int, kMaxPlayers> palive{};
    std::array<int, kMaxEnemies> ehp{};
    std::array<int, kMaxEnemies> emaxhp{};
    std::array<int, kMaxEnemies> estam{};
    std::array<int, kMaxEnemies> emaxstam{};
    std::array<int, kMaxEnemies> ealive{};
    std::array<int, kMaxPlayers> pstunned{};
    std::array<int, kMaxEnemies> estunned{};
    std::array<int, kMaxPlayers> inv_frag{};
    std::array<int, kMaxPlayers> store_frag{};
    /* Snapshot of player 0's inventory and storage contents for the side
     * panel; lets the grader see the memory-allocation work in real time. */
    std::array<int, kInventorySlots> p0_inv_slots{};
    std::array<int, kInventorySlots> p0_inv_block_size{};
    std::array<int, kStorageSlots> p0_storage_slots{};
    std::array<int, kStorageSlots> p0_storage_block_size{};
    std::array<int, 3> artifact_holder{};
    std::array<int, 3> artifact_present{};
    int drop_offer_status = 0;
    int drop_offer_weapon_id = 0;
    int game_over_reason = 0;
    int arbiter_pid = -1;
};

void Fail(const char* msg) {
    std::fprintf(stderr, "%s: %s\n", msg, std::strerror(errno));
    std::exit(1);
}

// Suppress non-fatal X11 errors (SFML 2.5 + XWayland compatibility).
// XWayland does not support X_ChangeActivePointerGrab, which SFML calls
// during window setup, causing a BadLength fatal error without this handler.
int IgnoreXError(Display* /*display*/, XErrorEvent* /*event*/) {
    return 0;
}

void DrawText(sf::RenderWindow& win, const sf::Font& font, const std::string& text, unsigned size, float x,
              float y, sf::Color color = sf::Color::White) {
    sf::Text t(text, font, size);
    t.setPosition(x, y);
    t.setFillColor(color);
    win.draw(t);
}

void DrawBar(sf::RenderWindow& win, float x, float y, float w, float h, float ratio, sf::Color color) {
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    sf::RectangleShape bg({w, h});
    bg.setPosition({x, y});
    bg.setFillColor(sf::Color(36, 40, 54));
    win.draw(bg);
    sf::RectangleShape fg({w * ratio, h});
    fg.setPosition({x, y});
    fg.setFillColor(color);
    win.draw(fg);
}

bool AliveEnemy(const SharedState* state, int idx) {
    return idx >= 0 && idx < state->enemy_count && state->enemy_alive[idx];
}

int NextAliveEnemy(const SharedState* state, int current, int direction) {
    if (state->enemy_count <= 0) return -1;
    int idx = current;
    for (int steps = 0; steps < state->enemy_count; ++steps) {
        idx = (idx + direction + state->enemy_count) % state->enemy_count;
        if (AliveEnemy(state, idx)) return idx;
    }
    return -1;
}

int FirstWeaponId(const SharedState* state, int player_idx) {
    if (player_idx < 0 || player_idx >= state->player_count) return 0;
    for (int i = 0; i < kInventorySlots; ++i) {
        if (state->inventory_slots[player_idx][i] > 0) return state->inventory_slots[player_idx][i];
    }
    return 0;
}
}  // namespace

int main() {
    XInitThreads();
    XSetErrorHandler(IgnoreXError);

    int fd = -1;
    for (int attempts = 0; attempts < 50; ++attempts) {
        fd = shm_open(kSharedMemoryName, O_RDWR, 0666);
        if (fd != -1) break;
        usleep(100 * 1000);
    }
    if (fd == -1) Fail("GUI shm_open failed");

    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) Fail("GUI mmap failed");
    auto* state = static_cast<SharedState*>(memory);

    sf::RenderWindow window(sf::VideoMode(kWindowW, kWindowH), "Chrono Rift - Shared Memory Battle View");
    window.setFramerateLimit(60);

    sf::Font font;
    bool loaded = false;
    const std::array<std::string, 3> font_paths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const auto& path : font_paths) {
        if (font.loadFromFile(path)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        std::fprintf(stderr, "GUI: failed to load any font (install fonts-dejavu-core or check paths)\n");
        return 1;
    }

    std::mutex render_mutex;
    Snapshot shared_snap;
    bool has_snapshot = false;
    int selected_enemy = 0;
    bool in_menu = true;
    int menu_index = 0;  // 0 start, 1 quit
    int selected_party_size = 1;
    std::string status_line = "Waiting for turns...";
    std::deque<std::string> action_log;
    std::atomic<bool> running{true};

    window.setActive(false);
    std::thread render_thread([&] {
        window.setActive(true);
        while (running.load() && window.isOpen()) {
            Snapshot snap;
            int local_selected_enemy = 0;
            bool local_in_menu = true;
            int local_menu_index = 0;
            int local_selected_party_size = 1;
            std::string local_status;
            std::deque<std::string> local_action_log;
            bool local_has_snapshot = false;
            {
                std::lock_guard<std::mutex> lk(render_mutex);
                snap = shared_snap;
                local_selected_enemy = selected_enemy;
                local_in_menu = in_menu;
                local_menu_index = menu_index;
                local_selected_party_size = selected_party_size;
                local_status = status_line;
                local_action_log = action_log;
                local_has_snapshot = has_snapshot;
            }

            window.clear(sf::Color(18, 20, 30));
            if (!local_has_snapshot) {
                DrawText(window, font, "Waiting for shared state...", 24, 460.f, 360.f);
                window.display();
                usleep(30 * 1000);
                continue;
            }

            if (local_in_menu) {
                sf::RectangleShape panel({760.f, 460.f});
                panel.setPosition({250.f, 140.f});
                panel.setFillColor(sf::Color(26, 30, 42));
                window.draw(panel);

                DrawText(window, font, "Chrono Rift", 48, 470.f, 175.f, sf::Color(195, 225, 255));
                DrawText(window, font, "Shared-Memory Tactical Mode", 22, 470.f, 232.f, sf::Color(215, 210, 180));
                DrawText(window, font, "Select Party Size: " + std::to_string(local_selected_party_size), 22, 320.f, 290.f,
                         sf::Color(185, 235, 255));
                DrawText(window, font, "Enemy Count (randomized): " + std::to_string(snap.enemy_count), 22, 320.f, 324.f);
                DrawText(window, font, "Roll Seed: " + std::to_string(snap.roll_number_seed), 22, 320.f, 358.f);
                DrawText(window, font, (local_menu_index == 0 ? "> Start Battle" : "  Start Battle"), 28, 470.f, 430.f,
                         local_menu_index == 0 ? sf::Color(255, 236, 140) : sf::Color(210, 210, 210));
                DrawText(window, font, (local_menu_index == 1 ? "> Quit" : "  Quit"), 28, 470.f, 468.f,
                         local_menu_index == 1 ? sf::Color(255, 180, 180) : sf::Color(210, 210, 210));
                window.display();
                usleep(30 * 1000);
                continue;
            }

            if (snap.game_running == 0 && snap.game_over_reason != 0) {
                std::string reason_text;
                sf::Color reason_color;
                if (snap.game_over_reason == 1) {
                    reason_text = "VICTORY! 10 enemies defeated.";
                    reason_color = sf::Color(100, 255, 100);
                } else if (snap.game_over_reason == 2) {
                    reason_text = "DEFEAT! All players have perished.";
                    reason_color = sf::Color(255, 100, 100);
                } else {
                    reason_text = "GAME OVER. Quit requested.";
                    reason_color = sf::Color(200, 200, 200);
                }
                DrawText(window, font, reason_text, 48, 250.f, 300.f, reason_color);
                DrawText(window, font, "Press ESC or Q to exit.", 24, 250.f, 380.f, sf::Color(255, 255, 255));
                window.display();
                usleep(50 * 1000);
                continue;
            }

            sf::RectangleShape arena({990.f, 580.f});
            arena.setPosition({20.f, 140.f});
            arena.setFillColor(sf::Color(36, 44, 64));
            window.draw(arena);

            DrawText(window, font, "Chrono Rift - Live Battle", 32, 24.f, 16.f, sf::Color(180, 220, 255));
            DrawText(window, font,
                     "HUD: Upper bar=HP  Lower=stamina | A|E|H|S|W|T|U  Arrows=enemies  ESC/Q quit  Y/N=drop pickup",
                     12, 24.f, 46.f, sf::Color(200, 200, 200));
            DrawText(window, font,
                     "Turn " + std::to_string(snap.current_turn) + " | Acting " + std::to_string(snap.acting_id) +
                         " | Kills " + std::to_string(snap.total_enemy_kills),
                     20, 24.f, 66.f);

            std::array<sf::Vector2f, kMaxPlayers> player_pos{};
            std::array<sf::Vector2f, kMaxEnemies> enemy_pos{};
            for (int i = 0; i < kMaxPlayers; ++i) player_pos[i] = {220.f + i * 95.f, 510.f + (i % 2) * 60.f};
            for (int i = 0; i < kMaxEnemies; ++i) {
                const float angle = (static_cast<float>(i) / std::max(1, snap.enemy_count)) * 2.f * kPi;
                enemy_pos[i] = {700.f + std::cos(angle) * 180.f, 320.f + std::sin(angle) * 130.f};
            }

            for (int i = 0; i < snap.enemy_count; ++i) {
                sf::CircleShape body(14.f);
                body.setOrigin(14.f, 14.f);
                body.setPosition(enemy_pos[i]);
                body.setFillColor(snap.ealive[i] ? sf::Color(214, 96, 96) : sf::Color(95, 95, 95));
                window.draw(body);
                if (i == local_selected_enemy) {
                    sf::CircleShape ring(20.f);
                    ring.setOrigin(20.f, 20.f);
                    ring.setPosition(enemy_pos[i]);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(255, 226, 110));
                    ring.setOutlineThickness(2.f);
                    window.draw(ring);
                }
                DrawBar(window, enemy_pos[i].x - 24.f, enemy_pos[i].y - 32.f, 48.f, 6.f,
                        snap.emaxhp[i] > 0 ? static_cast<float>(snap.ehp[i]) / static_cast<float>(snap.emaxhp[i]) : 0.f,
                        sf::Color(245, 128, 128));
                DrawBar(window, enemy_pos[i].x - 24.f, enemy_pos[i].y - 22.f, 48.f, 4.f,
                        snap.emaxstam[i] > 0 ? static_cast<float>(snap.estam[i]) / static_cast<float>(snap.emaxstam[i]) : 0.f,
                        sf::Color(138, 192, 255));
                if (snap.estunned[i] && snap.ealive[i]) {
                    DrawText(window, font, "STUN", 11, enemy_pos[i].x - 16.f, enemy_pos[i].y + 16.f,
                             sf::Color(255, 200, 80));
                }
            }

            for (int i = 0; i < snap.player_count; ++i) {
                sf::CircleShape body(15.f);
                body.setOrigin(15.f, 15.f);
                body.setPosition(player_pos[i]);
                body.setFillColor(snap.palive[i] ? sf::Color(96, 218, 128) : sf::Color(100, 100, 100));
                window.draw(body);
                if (snap.acting_side == 0 && snap.acting_id == 100 + i) {
                    sf::CircleShape ring(21.f);
                    ring.setOrigin(21.f, 21.f);
                    ring.setPosition(player_pos[i]);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(100, 255, 100));
                    ring.setOutlineThickness(2.f);
                    window.draw(ring);
                }
                DrawBar(window, player_pos[i].x - 25.f, player_pos[i].y - 32.f, 50.f, 6.f,
                        snap.pmax[i] > 0 ? static_cast<float>(snap.php[i]) / static_cast<float>(snap.pmax[i]) : 0.f,
                        sf::Color(92, 226, 140));
                DrawBar(window, player_pos[i].x - 25.f, player_pos[i].y - 22.f, 50.f, 4.f,
                        snap.pmaxstam[i] > 0 ? static_cast<float>(snap.pstam[i]) / static_cast<float>(snap.pmaxstam[i]) : 0.f,
                        sf::Color(180, 220, 255));
                if (snap.pstunned[i] && snap.palive[i]) {
                    DrawText(window, font, "STUN", 11, player_pos[i].x - 16.f, player_pos[i].y + 18.f,
                             sf::Color(255, 200, 80));
                }
            }

            sf::RectangleShape side({250.f, 580.f});
            side.setPosition({1010.f, 140.f});
            side.setFillColor(sf::Color(30, 30, 42));
            window.draw(side);
            DrawText(window, font, "Status", 22, 1028.f, 154.f);
            if (snap.drop_offer_status == 1) {
                DrawText(window, font,
                         "WEAPON DROP #" + std::to_string(snap.drop_offer_weapon_id) + ": Y=Take / N=Refuse", 13, 1028.f,
                         174.f, sf::Color(255, 226, 120));
            }
            DrawText(window, font, "Players Alive: " + std::to_string(snap.players_alive), 16, 1028.f, 190.f);
            DrawText(window, font, "Enemies Alive: " + std::to_string(snap.enemies_alive), 16, 1028.f, 214.f);
            DrawText(window, font, "Selected Enemy: E" + std::to_string(std::max(0, local_selected_enemy)), 14, 1028.f, 238.f);
            if (snap.player_count > 0) {
                DrawText(window, font,
                         "P0 Fragments inv/store: " + std::to_string(snap.inv_frag[0]) + "/" +
                             std::to_string(snap.store_frag[0]),
                         13, 1028.f, 258.f, sf::Color(180, 220, 255));
            }
            /* P0 inventory + storage contents (shows memory allocator output). */
            const std::array<std::string, 9> kWeaponNames = {
                "-", "Solar(10)", "Lunar(10)", "Halberd(7)", "Dagger(4)",
                "Tstaff(6)", "Axe(5)", "Frost(6)", "Stick(2)",
            };
            float lyA = 280.f;
            DrawText(window, font, "P0 Inventory (20 slots):", 13, 1028.f, lyA, sf::Color(180, 220, 255));
            lyA += 16.f;
            std::string inv_line;
            for (int s = 0; s < kInventorySlots; ++s) {
                if (snap.p0_inv_slots[s] > 0 && snap.p0_inv_block_size[s] > 0) {
                    int wid = snap.p0_inv_slots[s];
                    if (!inv_line.empty()) inv_line += ", ";
                    inv_line += (wid >= 1 && wid <= 8) ? kWeaponNames[wid] : ("?" + std::to_string(wid));
                }
            }
            if (inv_line.empty()) inv_line = "(empty)";
            DrawText(window, font, inv_line, 12, 1028.f, lyA, sf::Color(210, 230, 200));
            lyA += 18.f;
            DrawText(window, font, "P0 Storage (20 slots):", 13, 1028.f, lyA, sf::Color(180, 220, 255));
            lyA += 16.f;
            std::string sto_line;
            for (int s = 0; s < kStorageSlots; ++s) {
                if (snap.p0_storage_slots[s] > 0 && snap.p0_storage_block_size[s] > 0) {
                    int wid = snap.p0_storage_slots[s];
                    if (!sto_line.empty()) sto_line += ", ";
                    sto_line += (wid >= 1 && wid <= 8) ? kWeaponNames[wid] : ("?" + std::to_string(wid));
                }
            }
            if (sto_line.empty()) sto_line = "(empty)";
            DrawText(window, font, sto_line, 12, 1028.f, lyA, sf::Color(210, 230, 200));
            lyA += 22.f;
            /* Artifact lock table. */
            DrawText(window, font, "Artifacts:", 13, 1028.f, lyA, sf::Color(180, 220, 255));
            lyA += 16.f;
            const std::array<std::string, 3> kArtNames = {"Solar", "Lunar", "Eclipse"};
            for (int a = 0; a < 3; ++a) {
                std::string line = kArtNames[a] + ": ";
                if (!snap.artifact_present[a]) line += "absent";
                else if (snap.artifact_holder[a] < 0) line += "free";
                else line += "held by " + std::to_string(snap.artifact_holder[a]);
                DrawText(window, font, line, 12, 1028.f, lyA, sf::Color(220, 220, 180));
                lyA += 14.f;
            }
            DrawText(window, font, local_status, 13, 1028.f, lyA + 6.f, sf::Color(230, 210, 150));
            float ly = lyA + 30.f;
            for (const auto& line : local_action_log) {
                DrawText(window, font, line, 12, 1028.f, ly, sf::Color(220, 220, 220));
                ly += 16.f;
                if (ly > 700.f) break;
            }

            window.display();
            usleep(30 * 1000);
        }
    });

    while (running.load() && window.isOpen()) {
        sf::Event event{};
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                running.store(false);
                window.close();
                break;
            }
            if (event.type != sf::Event::KeyPressed) continue;

            if (in_menu) {
                if (event.key.code == sf::Keyboard::Up || event.key.code == sf::Keyboard::W ||
                    event.key.code == sf::Keyboard::Down || event.key.code == sf::Keyboard::S) {
                    menu_index = (menu_index + 1) % 2;
                } else if (event.key.code == sf::Keyboard::Left || event.key.code == sf::Keyboard::A) {
                    selected_party_size = std::max(1, selected_party_size - 1);
                } else if (event.key.code == sf::Keyboard::Right || event.key.code == sf::Keyboard::D) {
                    selected_party_size = std::min(kMaxPlayers, selected_party_size + 1);
                } else if (event.key.code == sf::Keyboard::Enter || event.key.code == sf::Keyboard::Space) {
                    sem_wait(&state->state_lock);
                    if (menu_index == 0) {
                        state->setup_party_size = selected_party_size;
                        state->setup_phase = 0;
                        in_menu = false;
                        status_line = "Battle started with party size " + std::to_string(selected_party_size) + ".";
                    } else {
                        state->quit_requested = 1;
                        sem_post(&state->turn_signal_hip);
                        sem_post(&state->turn_signal_asp);
                        status_line = "Quit requested from menu.";
                        running.store(false);
                        window.close();
                    }
                    sem_post(&state->state_lock);
                }
                continue;
            }

            sem_wait(&state->state_lock);
            const bool game_running = state->game_running != 0;
            const int over_reason = state->game_over_reason;
            if (!game_running && over_reason != 0 && (event.key.code == sf::Keyboard::Escape || event.key.code == sf::Keyboard::Q)) {
                sem_post(&state->state_lock);
                running.store(false);
                window.close();
                continue;
            }
            if (!game_running) {
                sem_post(&state->state_lock);
                continue;
            }
            if ((event.key.code == sf::Keyboard::Y || event.key.code == sf::Keyboard::N) &&
                state->drop_offer_status == 1) {
                state->drop_offer_status =
                    event.key.code == sf::Keyboard::Y ? 2 : 3;
                status_line = state->drop_offer_status == 2 ? "Pickup accepted." : "Pickup declined (NPC eligible).";
                sem_post(&state->state_lock);
                continue;
            }
            if (event.key.code == sf::Keyboard::Escape || event.key.code == sf::Keyboard::Q) {
                state->quit_requested = 1;
                if (state->arbiter_pid > 0) {
                    kill(state->arbiter_pid, SIGTERM);
                }
                sem_post(&state->turn_signal_hip);
                sem_post(&state->turn_signal_asp);
                status_line = "Quit requested.";
                sem_post(&state->state_lock);
                continue;
            }
            if (event.key.code == sf::Keyboard::Left) {
                selected_enemy = NextAliveEnemy(state, std::max(0, selected_enemy), -1);
                sem_post(&state->state_lock);
                continue;
            }
            if (event.key.code == sf::Keyboard::Right) {
                selected_enemy = NextAliveEnemy(state, std::max(0, selected_enemy), +1);
                sem_post(&state->state_lock);
                continue;
            }

            const int actor_id = state->acting_id;
            const bool player_turn = state->acting_side == 0 && actor_id >= 100;
            const int player_idx = actor_id - 100;
            if (!player_turn || player_idx < 0 || player_idx >= state->player_count || !state->player_alive[player_idx]) {
                sem_post(&state->state_lock);
                continue;
            }

            if (!AliveEnemy(state, selected_enemy)) selected_enemy = NextAliveEnemy(state, -1, +1);
            if (selected_enemy < 0) selected_enemy = 0;

            ActionPacket action{};
            action.actor_id = actor_id;
            action.target_id = 200 + selected_enemy;
            action.weapon_id = 0;
            action.aux = 0;
            action.value = 0;
            action.action = ActionType::Skip;
            bool submit = false;
            if (event.key.code == sf::Keyboard::A) {
                action.action = ActionType::Strike;
                action.value = state->player_damage[player_idx];
                submit = true;
                status_line = "Submitted Strike.";
            } else if (event.key.code == sf::Keyboard::E) {
                action.action = ActionType::Exhaust;
                action.value = state->player_damage[player_idx];
                submit = true;
                status_line = "Submitted Exhaust.";
            } else if (event.key.code == sf::Keyboard::H) {
                action.action = ActionType::Heal;
                action.target_id = actor_id;
                submit = true;
                status_line = "Submitted Heal.";
            } else if (event.key.code == sf::Keyboard::S) {
                action.action = ActionType::Skip;
                action.target_id = -1;
                submit = true;
                status_line = "Submitted Skip.";
            } else if (event.key.code == sf::Keyboard::W) {
                action.action = ActionType::UseWeapon;
                action.weapon_id = FirstWeaponId(state, player_idx);
                submit = true;
                status_line = action.weapon_id > 0 ? "Submitted UseWeapon." : "No weapon in inventory.";
            } else if (event.key.code == sf::Keyboard::T) {
                action.action = ActionType::SwapIn;
                action.target_id = actor_id;
                submit = true;
                status_line = "Submitted SwapIn.";
            } else if (event.key.code == sf::Keyboard::U) {
                action.action = ActionType::Ultimate;
                action.target_id = -1;
                submit = true;
                status_line = "Submitted Ultimate.";
            }

            if (submit) {
                state->gui_action_packet = action;
                state->gui_action_pending = 1;
                std::ostringstream os;
                os << "P" << player_idx << " action " << static_cast<int>(action.action) << " -> " << action.target_id;
                action_log.push_front(os.str());
                if (action_log.size() > 10) action_log.pop_back();
            }
            sem_post(&state->state_lock);
        }

        Snapshot snap;
        sem_wait(&state->state_lock);
        snap.game_running = state->game_running;
        snap.acting_id = state->acting_id;
        snap.acting_side = state->acting_side;
        snap.current_turn = state->current_turn;
        snap.roll_number_seed = state->roll_number_seed;
        snap.setup_phase = state->setup_phase;
        snap.setup_party_size = state->setup_party_size;
        snap.players_alive = state->players_alive;
        snap.enemies_alive = state->enemies_alive;
        snap.total_enemy_kills = state->total_enemy_kills;
        snap.player_count = state->player_count;
        snap.enemy_count = state->enemy_count;
        for (int i = 0; i < kMaxPlayers; ++i) {
            snap.php[i] = state->player_hp[i];
            snap.pmax[i] = state->player_max_hp[i];
            snap.pstam[i] = state->player_stamina[i];
            snap.pmaxstam[i] = state->player_max_stamina[i];
            snap.palive[i] = state->player_alive[i];
            snap.pstunned[i] = state->player_stunned[i];
            snap.inv_frag[i] = state->inventory_fragments[i];
            snap.store_frag[i] = state->storage_fragments[i];
        }
        if (state->player_count > 0) {
            for (int s = 0; s < kInventorySlots; ++s) {
                snap.p0_inv_slots[s] = state->inventory_slots[0][s];
                snap.p0_inv_block_size[s] = state->inventory_block_size[0][s];
            }
            for (int s = 0; s < kStorageSlots; ++s) {
                snap.p0_storage_slots[s] = state->storage_slots[0][s];
                snap.p0_storage_block_size[s] = state->storage_block_size[0][s];
            }
        }
        for (int i = 0; i < kMaxEnemies; ++i) {
            snap.ehp[i] = state->enemy_hp[i];
            snap.emaxhp[i] = state->enemy_max_hp[i];
            snap.estam[i] = state->enemy_stamina[i];
            snap.emaxstam[i] = state->enemy_max_stamina[i];
            snap.ealive[i] = state->enemy_alive[i];
            snap.estunned[i] = state->enemy_stunned[i];
        }
        for (int i = 0; i < 3; ++i) {
            snap.artifact_holder[i] = state->artifact_holder[i];
            snap.artifact_present[i] = state->artifact_present[i];
        }
        snap.drop_offer_status = state->drop_offer_status;
        snap.drop_offer_weapon_id = state->drop_offer_weapon_id;
        snap.game_over_reason = state->game_over_reason;
        snap.arbiter_pid = state->arbiter_pid;
        sem_post(&state->state_lock);

        {
            std::lock_guard<std::mutex> lk(render_mutex);
            shared_snap = snap;
            has_snapshot = true;
        }

        usleep(8 * 1000);
    }

    running.store(false);
    if (render_thread.joinable()) render_thread.join();
    munmap(memory, sizeof(SharedState));
    close(fd);
    return 0;
}
