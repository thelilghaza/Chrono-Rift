#include "../common/game_shared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <climits>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t g_terminate_requested = 0;
volatile std::sig_atomic_t g_resume_asp_requested = 0;

constexpr int kMaxReaped = 16;
volatile std::sig_atomic_t g_reaped_pids[kMaxReaped];
volatile std::sig_atomic_t g_reaped_count = 0;
/** ASP frozen by Ultimate only; not shared with ASP (Arbiter-local, signal-driven pause). */
bool g_asp_frozen_for_ultimate = false;

bool g_hip_under_stun_stop = false;
bool g_asp_under_stun_stop = false;

enum class StunResumeTarget : int { HipProc = 0, AspProc = 1 };

struct StunResumeJob {
    int pid{};
    StunResumeTarget side{};
    std::chrono::steady_clock::time_point resume_at{};
};
std::mutex g_stun_queue_mu;
std::condition_variable g_stun_queue_cv;
std::deque<StunResumeJob> g_stun_jobs;
std::atomic<bool> g_stun_worker_stop{false};
std::thread g_stun_resume_worker;

std::mt19937* g_rng_ptr = nullptr;

void SafeKill(int pid, int sig);

void OnSigTerm(int) { g_terminate_requested = 1; }
void OnSigAlrm(int) { g_resume_asp_requested = 1; }
void OnSigChld(int) {
    int saved_errno = errno;
    pid_t p;
    while ((p = waitpid(-1, nullptr, WNOHANG)) > 0) {
        if (g_reaped_count < kMaxReaped) {
            g_reaped_pids[g_reaped_count++] = p;
        }
    }
    errno = saved_errno;
}

void StunResumeWorkerRun() {
    while (true) {
        std::unique_lock<std::mutex> lk(g_stun_queue_mu);
        if (g_stun_worker_stop.load() && g_stun_jobs.empty()) {
            lk.unlock();
            return;
        }
        if (g_stun_jobs.empty()) {
            g_stun_queue_cv.wait(lk, [&] {
                return g_stun_worker_stop.load() || !g_stun_jobs.empty();
            });
            continue;
        }
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        auto earliest = std::min_element(g_stun_jobs.begin(), g_stun_jobs.end(),
                                         [](const StunResumeJob& a, const StunResumeJob& b) {
                                             return a.resume_at < b.resume_at;
                                         });
        if (earliest->resume_at > now) {
            g_stun_queue_cv.wait_until(lk, earliest->resume_at);
            continue;
        }
        const int pid = earliest->pid;
        const StunResumeTarget side = earliest->side;
        g_stun_jobs.erase(earliest);
        lk.unlock();

        SafeKill(pid, SIGCONT);
        if (side == StunResumeTarget::HipProc) {
            g_hip_under_stun_stop = false;
        } else {
            g_asp_under_stun_stop = false;
        }
    }
}

void StartStunResumeWorker() {
    if (g_stun_resume_worker.joinable()) return;
    g_stun_worker_stop.store(false);
    g_stun_resume_worker = std::thread(StunResumeWorkerRun);
}

void StopStunResumeWorker() {
    {
        std::lock_guard<std::mutex> lk(g_stun_queue_mu);
        g_stun_worker_stop.store(true);
    }
    g_stun_queue_cv.notify_all();
    if (g_stun_resume_worker.joinable()) {
        g_stun_resume_worker.join();
    }
    std::lock_guard<std::mutex> lk2(g_stun_queue_mu);
    for (const auto& j : g_stun_jobs) {
        SafeKill(j.pid, SIGCONT);
    }
    g_stun_jobs.clear();
    g_hip_under_stun_stop = false;
    g_asp_under_stun_stop = false;
}

void QueueProcessResumeAfterStun(int pid, StunResumeTarget side) {
    const auto resume_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    {
        std::lock_guard<std::mutex> lk(g_stun_queue_mu);
        for (auto it = g_stun_jobs.begin(); it != g_stun_jobs.end();) {
            if (it->pid == pid && it->side == side) {
                it = g_stun_jobs.erase(it);
            } else {
                ++it;
            }
        }
        g_stun_jobs.push_back(StunResumeJob{pid, side, resume_at});
    }
    g_stun_queue_cv.notify_one();
}

void PulseAsyncStunNotice(int pid) {
    SafeKill(pid, SIGUSR1);
}

void DeliverStunFreeze(int pid, StunResumeTarget side) {
    SafeKill(pid, SIGSTOP);
    QueueProcessResumeAfterStun(pid, side);
}

void SetupSignalHandlers() {
    struct sigaction sa_term {};
    sa_term.sa_handler = OnSigTerm;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa_term, nullptr);

    struct sigaction sa_chld {};
    sa_chld.sa_handler = OnSigChld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, nullptr);

    struct sigaction sa_alrm {};
    sa_alrm.sa_handler = OnSigAlrm;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa_alrm, nullptr);
}

void SafeKill(int pid, int sig) {
    if (pid > 0) {
        kill(pid, sig);
    }
}

void Fail(const char* msg) {
    std::cerr << msg << ": " << std::strerror(errno) << "\n";
    std::exit(1);
}

int ParseEnvInt(const char* key, int fallback) {
    const char* raw = std::getenv(key);
    if (!raw) return fallback;
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') return fallback;
    return static_cast<int>(value);
}

int WeaponDamage(int weapon_id) {
    switch (weapon_id) {
        case 1:  // Solar Core
            return 95;
        case 2:  // Lunar Blade
            return 90;
        case 3:  // Iron Halberd
            return 55;
        case 4:  // Venom Dagger
            return 30;
        case 5:  // Thunderstaff
            return 50;
        case 6:  // Obsidian Axe
            return 45;
        case 7:  // Frostbow
            return 48;
        case 8:  // Splinter Stick
            return 12;
        default:
            return 0;
    }
}

int NowSec() { return static_cast<int>(std::time(nullptr)); }

bool IsStunnedUntil(int stun_until) { return stun_until > 0 && NowSec() < stun_until; }

void ReleaseActorArtifacts(SharedState* state, int actor_id) {
    for (int a = 0; a < 3; ++a) {
        if (state->artifact_holder[a] == actor_id) state->artifact_holder[a] = -1;
    }
    if (actor_id >= 0 && actor_id < 300) state->actor_waiting_artifact[actor_id] = -1;
}

bool TryAcquireArtifact(SharedState* state, int actor_id, int artifact_id) {
    if (artifact_id < 0 || artifact_id > 2 || !state->artifact_present[artifact_id]) return false;
    const int holder = state->artifact_holder[artifact_id];
    if (holder == -1 || holder == actor_id) {
        state->artifact_holder[artifact_id] = actor_id;
        if (actor_id >= 0 && actor_id < 300) state->actor_waiting_artifact[actor_id] = -1;
        return true;
    }
    if (actor_id >= 0 && actor_id < 300) state->actor_waiting_artifact[actor_id] = artifact_id;
    return false;
}

int RequiredSlotsForWeapon(int weapon_id) {
    switch (weapon_id) {
        case 1:   // Solar Core
            return 10;
        case 2:   // Lunar Blade
            return 10;
        case 3:   // Iron Halberd
            return 7;
        case 4:   // Venom Dagger
            return 4;
        case 5:   // Thunderstaff
            return 6;
        case 6:   // Obsidian Axe
            return 5;
        case 7:   // Frostbow
            return 6;
        case 8:   // Splinter Stick
            return 2;
        default:
            return 1;
    }
}

void UpdateFragmentCountFor(int* slots, int slot_count, int* out_fragments) {
    int fragments = 0;
    bool in_free_run = false;
    for (int i = 0; i < slot_count; ++i) {
        const bool is_free = slots[i] == 0;
        if (is_free && !in_free_run) {
            ++fragments;
            in_free_run = true;
        } else if (!is_free) {
            in_free_run = false;
        }
    }
    *out_fragments = fragments;
}

void RefreshPlayerMemoryLayout(SharedState* state, int player_idx) {
    if (player_idx < 0 || player_idx >= kMaxPlayers) return;
    UpdateFragmentCountFor(state->inventory_slots[player_idx], kInventorySlots, &state->inventory_fragments[player_idx]);
    UpdateFragmentCountFor(state->storage_slots[player_idx], kStorageSlots, &state->storage_fragments[player_idx]);
}

bool PlaceInFirstContiguousFree(int* slots, int* block_sizes, int slot_count, int item_id, int size) {
    if (size <= 0 || size > slot_count) return false;
    for (int i = 0; i + size <= slot_count; ++i) {
        bool can_place = true;
        for (int j = 0; j < size; ++j) {
            if (slots[i + j] != 0) {
                can_place = false;
                i += j;
                break;
            }
        }
        if (!can_place) continue;
        slots[i] = item_id;
        block_sizes[i] = size;
        for (int j = 1; j < size; ++j) {
            slots[i + j] = -1;
            block_sizes[i + j] = 0;
        }
        return true;
    }
    return false;
}

bool PlaceInStorageSized(SharedState* state, int player_idx, int item_id, int slot_size) {
    return PlaceInFirstContiguousFree(state->storage_slots[player_idx], state->storage_block_size[player_idx],
                                      kStorageSlots, item_id, slot_size);
}

bool PlaceInStorage(SharedState* state, int player_idx, int item_id) {
    return PlaceInStorageSized(state, player_idx, item_id, RequiredSlotsForWeapon(item_id));
}

void PushDroppedWeapon(SharedState* state, int weapon_id) {
    if (state->dropped_weapon_count >= static_cast<int>(sizeof(state->dropped_weapon_ids) / sizeof(state->dropped_weapon_ids[0]))) return;
    state->dropped_weapon_ids[state->dropped_weapon_count++] = weapon_id;
}

void CompactSlots(int* slots, int* block_sizes, int slot_count) {
    int compact_slots[kStorageSlots > kInventorySlots ? kStorageSlots : kInventorySlots] = {};
    int compact_block_sizes[kStorageSlots > kInventorySlots ? kStorageSlots : kInventorySlots] = {};
    int write = 0;
    for (int i = 0; i < slot_count; ++i) {
        if (slots[i] <= 0) continue;
        const int item_id = slots[i];
        const int size = std::max(1, block_sizes[i]);
        if (write + size > slot_count) break;
        compact_slots[write] = item_id;
        compact_block_sizes[write] = size;
        for (int j = 1; j < size; ++j) {
            compact_slots[write + j] = -1;
            compact_block_sizes[write + j] = 0;
        }
        write += size;
    }
    for (int i = 0; i < slot_count; ++i) {
        slots[i] = compact_slots[i];
        block_sizes[i] = compact_block_sizes[i];
    }
}

void CompactPlayerMemory(SharedState* state, int player_idx) {
    if (player_idx < 0 || player_idx >= state->player_count) return;
    CompactSlots(state->inventory_slots[player_idx], state->inventory_block_size[player_idx], kInventorySlots);
    CompactSlots(state->storage_slots[player_idx], state->storage_block_size[player_idx], kStorageSlots);
    RefreshPlayerMemoryLayout(state, player_idx);
}

bool PlayerInventoryHasArtifactsForUltimate(SharedState* state, int player_idx) {
    if (player_idx < 0 || player_idx >= kMaxPlayers) return false;
    bool solar = false;
    bool lunar = false;
    for (int i = 0; i < kInventorySlots; ++i) {
        const int cell = state->inventory_slots[player_idx][i];
        if (cell <= 0 || state->inventory_block_size[player_idx][i] <= 0) continue;
        if (cell == 1) solar = true;
        if (cell == 2) lunar = true;
    }
    return solar && lunar;
}

bool EnsureWeaponPlacementInInventory(SharedState* state, int player_idx, int weapon_id, int weapon_size,
                                      int /*iterations_limit*/ = 96) {
    if (weapon_size <= 0 || weapon_size > kInventorySlots) return false;

    int* inv = state->inventory_slots[player_idx];
    int* blocks = state->inventory_block_size[player_idx];

    /* Fast path: a contiguous free run of the right size already exists. */
    if (PlaceInFirstContiguousFree(inv, blocks, kInventorySlots, weapon_id, weapon_size)) {
        RefreshPlayerMemoryLayout(state, player_idx);
        return true;
    }

    /* Spec §6: "The allocator must only swap out as many weapons as are
     * necessary." Strategy: for every candidate window [start, start+size),
     * count how many block-heads have a span that intersects the window.
     * Pick the window with the minimum count and evict only those blocks. */
    int best_window_start = -1;
    int best_eviction_heads[kInventorySlots] = {0};
    int best_eviction_count = INT_MAX;

    for (int start = 0; start + weapon_size <= kInventorySlots; ++start) {
        int eviction_heads[kInventorySlots] = {0};
        int eviction_count = 0;
        const int win_end = start + weapon_size;
        for (int h = 0; h < kInventorySlots; ++h) {
            if (inv[h] <= 0) continue;       // not a block head (free or continuation)
            if (blocks[h] <= 0) continue;    // not a block head (continuation slot)
            const int head_end = h + blocks[h];
            if (h < win_end && head_end > start) {
                eviction_heads[eviction_count++] = h;
            }
        }
        if (eviction_count < best_eviction_count) {
            best_eviction_count = eviction_count;
            best_window_start = start;
            for (int e = 0; e < eviction_count; ++e) best_eviction_heads[e] = eviction_heads[e];
        }
    }

    if (best_window_start < 0 || best_eviction_count == INT_MAX) return false;

    /* Try to evict the chosen blocks to long-term storage. Bail if storage
     * cannot accommodate any of them (caller will fall through). */
    for (int e = 0; e < best_eviction_count; ++e) {
        const int h = best_eviction_heads[e];
        const int wid = inv[h];
        const int sz = blocks[h];
        if (!PlaceInStorageSized(state, player_idx, wid, sz)) {
            return false;
        }
        for (int j = 0; j < sz && h + j < kInventorySlots; ++j) {
            inv[h + j] = 0;
            blocks[h + j] = 0;
        }
    }

    if (PlaceInFirstContiguousFree(inv, blocks, kInventorySlots, weapon_id, weapon_size)) {
        RefreshPlayerMemoryLayout(state, player_idx);
        return true;
    }
    /* Fallback: compact and retry. Should be unreachable when the eviction
     * window analysis above is correct, but cheap insurance. */
    CompactSlots(inv, blocks, kInventorySlots);
    if (PlaceInFirstContiguousFree(inv, blocks, kInventorySlots, weapon_id, weapon_size)) {
        RefreshPlayerMemoryLayout(state, player_idx);
        return true;
    }
    return false;
}

void SaveStorageState(SharedState* state) {
    std::ofstream out("savegame.txt", std::ios::trunc);
    if (!out) return;
    out << "CHRONO_RIFT_SAVE_V2\n";
    out << state->player_count << "\n";
    for (int p = 0; p < state->player_count; ++p) {
        for (int i = 0; i < kInventorySlots; ++i) {
            out << state->inventory_slots[p][i] << ":" << state->inventory_block_size[p][i];
            if (i + 1 < kInventorySlots) out << ",";
        }
        out << "\n";
        for (int i = 0; i < kStorageSlots; ++i) {
            out << state->storage_slots[p][i] << ":" << state->storage_block_size[p][i];
            if (i + 1 < kStorageSlots) out << ",";
        }
        out << "\n";
    }
}

void LoadStorageState(SharedState* state) {
    std::ifstream in("savegame.txt");
    if (!in) return;
    std::string header;
    std::getline(in, header);
    if (header != "CHRONO_RIFT_SAVE_V2") return;
    int players = 0;
    in >> players;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    const int pcount = std::min(players, state->player_count);
    for (int p = 0; p < pcount; ++p) {
        std::string inv_line;
        std::string store_line;
        if (!std::getline(in, inv_line)) break;
        if (!std::getline(in, store_line)) break;
        {
            std::stringstream ss(inv_line);
            std::string token;
            int i = 0;
            while (std::getline(ss, token, ',') && i < kInventorySlots) {
                const auto sep = token.find(':');
                if (sep != std::string::npos) {
                    state->inventory_slots[p][i] = std::strtol(token.substr(0, sep).c_str(), nullptr, 10);
                    state->inventory_block_size[p][i] = std::strtol(token.substr(sep + 1).c_str(), nullptr, 10);
                }
                ++i;
            }
        }
        {
            std::stringstream ss(store_line);
            std::string token;
            int i = 0;
            while (std::getline(ss, token, ',') && i < kStorageSlots) {
                const auto sep = token.find(':');
                if (sep != std::string::npos) {
                    state->storage_slots[p][i] = std::strtol(token.substr(0, sep).c_str(), nullptr, 10);
                    state->storage_block_size[p][i] = std::strtol(token.substr(sep + 1).c_str(), nullptr, 10);
                }
                ++i;
            }
        }
        RefreshPlayerMemoryLayout(state, p);
    }
}

void ApplyStun(SharedState* state, int target_id) {
    const int now = NowSec();
    if (target_id >= 100 && target_id < 200) {
        const int idx = target_id - 100;
        if (idx < 0 || idx >= state->player_count || !state->player_alive[idx]) return;
        state->player_stunned[idx] = 1;
        state->player_stun_until[idx] = now + 3;
        /* Stamina is preserved across stun; scheduler skips stunned actors via stun_until wall clock. */
        if (state->hip_pid > 0) {
            const int pid = state->hip_pid;
            const bool hip_first_hit = !g_hip_under_stun_stop;
            PulseAsyncStunNotice(pid);
            if (hip_first_hit) {
                DeliverStunFreeze(pid, StunResumeTarget::HipProc);
                g_hip_under_stun_stop = true;
            } else {
                QueueProcessResumeAfterStun(pid, StunResumeTarget::HipProc);
            }
        }
    } else if (target_id >= 200 && target_id < 300) {
        const int idx = target_id - 200;
        if (idx < 0 || idx >= state->enemy_count || !state->enemy_alive[idx]) return;
        state->enemy_stunned[idx] = 1;
        state->enemy_stun_until[idx] = now + 3;
        /* Same stamina-preservation semantics for NPC stun bookkeeping. */
        if (g_asp_frozen_for_ultimate || state->asp_pid <= 0) {
            return;
        }
        const int pid = state->asp_pid;
        const bool asp_first_hit = !g_asp_under_stun_stop;
        PulseAsyncStunNotice(pid);
        if (asp_first_hit) {
            DeliverStunFreeze(pid, StunResumeTarget::AspProc);
            g_asp_under_stun_stop = true;
        } else {
            QueueProcessResumeAfterStun(pid, StunResumeTarget::AspProc);
        }
    }
}

void ClampAliveFlags(SharedState* state) {
    for (int i = 0; i < state->player_count; ++i) {
        if (state->player_alive[i] && state->player_hp[i] <= 0) {
            state->player_hp[i] = 0;
            state->player_alive[i] = 0;
            --state->players_alive;
            ReleaseActorArtifacts(state, 100 + i);
        }
    }
    for (int i = 0; i < state->enemy_count; ++i) {
        if (state->enemy_alive[i] && state->enemy_hp[i] <= 0) {
            state->enemy_hp[i] = 0;
            state->enemy_alive[i] = 0;
            --state->enemies_alive;
            ++state->total_enemy_kills;
            ReleaseActorArtifacts(state, 200 + i);
            const int npc_had_weapon = state->enemy_carrying_weapon[i];
            state->enemy_carrying_weapon[i] = 0;
            if (npc_had_weapon <= 0 && state->player_count > 0 && state->drop_offer_status != 1) {
                std::uniform_int_distribution<int> drop_dist(0, 99);
                const int drop_roll = g_rng_ptr ? drop_dist(*g_rng_ptr) : 50;
                if (drop_roll < 35) {
                    std::uniform_int_distribution<int> weapon_dist(3, 8);
                    const int dropped_weapon = g_rng_ptr ? weapon_dist(*g_rng_ptr) : 3;
                    state->drop_offer_weapon_id = dropped_weapon;
                    state->drop_offer_status = 1;
                }
            }
        }
    }
}

void ApplyAction(SharedState* state, const ActionPacket& action) {
    if (action.action == ActionType::Strike || action.action == ActionType::UseWeapon) {
        if (action.actor_id >= 100 && action.target_id >= 200) {
            const int enemy_idx = action.target_id - 200;
            if (enemy_idx >= 0 && enemy_idx < state->enemy_count && state->enemy_alive[enemy_idx]) {
                if (state->artifact_present[2]) {
                    TryAcquireArtifact(state, action.actor_id, 2);
                }
                int damage = action.action == ActionType::UseWeapon ? WeaponDamage(action.weapon_id) : action.value;
                if (state->artifact_holder[2] == action.actor_id) damage += 20;  // Eclipse relic bonus.
                if (action.action == ActionType::UseWeapon && (action.weapon_id == 1 || action.weapon_id == 2)) {
                    const int artifact_id = action.weapon_id == 1 ? 0 : 1;
                    if (!TryAcquireArtifact(state, action.actor_id, artifact_id)) damage = 0;
                }
                state->enemy_hp[enemy_idx] -= damage;
                if (action.action == ActionType::UseWeapon && damage >= 90) ApplyStun(state, action.target_id);
            }
        } else if (action.actor_id >= 200 && action.target_id >= 100) {
            const int enemy_idx_from_actor = action.actor_id - 200;
            const int player_idx = action.target_id - 100;
            if (player_idx >= 0 && player_idx < state->player_count && state->player_alive[player_idx]) {
                if (state->artifact_present[2]) {
                    TryAcquireArtifact(state, action.actor_id, 2);
                }
                int inflicted = action.value;
                if (action.action == ActionType::UseWeapon && action.weapon_id > 0) {
                    inflicted = WeaponDamage(action.weapon_id);
                }
                if (state->artifact_holder[2] == action.actor_id) inflicted += 20;
                state->player_hp[player_idx] -= inflicted;
                if (inflicted >= 20) ApplyStun(state, action.target_id);
                if (action.action == ActionType::UseWeapon && action.weapon_id > 0 &&
                    enemy_idx_from_actor >= 0 && enemy_idx_from_actor < state->enemy_count) {
                    state->enemy_carrying_weapon[enemy_idx_from_actor] = action.weapon_id;
                }
            }
        }
    } else if (action.action == ActionType::Ultimate && action.actor_id >= 100 && action.actor_id < 200) {
        const int player_idx = action.actor_id - 100;
        if (player_idx >= 0 && player_idx < state->player_count &&
            PlayerInventoryHasArtifactsForUltimate(state, player_idx)) {
            for (int i = 0; i < state->enemy_count; ++i) {
                if (state->enemy_alive[i]) {
                    state->enemy_hp[i] -= 120;
                }
            }
            if (state->asp_pid > 0 && !g_asp_frozen_for_ultimate) {
                PulseAsyncStunNotice(state->asp_pid);
                SafeKill(state->asp_pid, SIGSTOP);
                g_asp_frozen_for_ultimate = true;
                alarm(10);
            }
            state->artifact_holder[0] = -1;
            state->artifact_holder[1] = -1;
            state->actor_waiting_artifact[action.actor_id] = -1;
        }
    } else if (action.action == ActionType::Exhaust && action.actor_id >= 100 && action.target_id >= 200) {
        const int enemy_idx = action.target_id - 200;
        if (enemy_idx >= 0 && enemy_idx < state->enemy_count && state->enemy_alive[enemy_idx]) {
            state->enemy_stamina[enemy_idx] -= action.value;
            if (state->enemy_stamina[enemy_idx] < 0) state->enemy_stamina[enemy_idx] = 0;
        }
    } else if (action.action == ActionType::Heal && action.actor_id >= 100) {
        const int player_idx = action.actor_id - 100;
        if (player_idx >= 0 && player_idx < state->player_count && state->player_alive[player_idx]) {
            const int heal_amount = (state->player_max_hp[player_idx] + 9) / 10;
            state->player_hp[player_idx] += heal_amount;
            if (state->player_hp[player_idx] > state->player_max_hp[player_idx]) {
                state->player_hp[player_idx] = state->player_max_hp[player_idx];
            }
        }
    } else if (action.action == ActionType::SwapIn && action.actor_id >= 100) {
        const int player_idx = action.actor_id - 100;
        if (player_idx >= 0 && player_idx < state->player_count) {
            int source_slot = -1;
            int weapon_id = 0;
            for (int s = 0; s < kStorageSlots; ++s) {
                if (state->storage_slots[player_idx][s] > 0) {
                    source_slot = s;
                    weapon_id = state->storage_slots[player_idx][s];
                    break;
                }
            }
            if (source_slot >= 0 && weapon_id > 0) {
                const int required = RequiredSlotsForWeapon(weapon_id);
                const bool placed =
                    EnsureWeaponPlacementInInventory(state, player_idx, weapon_id, required);
                if (placed) {
                    const int clear_span = std::max(1, state->storage_block_size[player_idx][source_slot]);
                    for (int j = 0; j < clear_span && source_slot + j < kStorageSlots; ++j) {
                        state->storage_slots[player_idx][source_slot + j] = 0;
                        state->storage_block_size[player_idx][source_slot + j] = 0;
                    }
                }
                CompactPlayerMemory(state, player_idx);
            }
        }
    }
    ClampAliveFlags(state);
}

void PumpDropOfferHandshake(SharedState* state) {
    /* Spec §6/§10: if the player does not pick up, the NPC is guaranteed to.
     * We enforce this with a 5-second player decision window so the game
     * never stalls on an unattended drop prompt. */
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    while (!g_terminate_requested && state->quit_requested == 0) {
        sem_wait(&state->state_lock);
        const int drop_status = state->drop_offer_status;
        sem_post(&state->state_lock);
        if (drop_status != 1) {
            break;
        }
        if (clock::now() >= deadline) {
            sem_wait(&state->state_lock);
            if (state->drop_offer_status == 1) {
                state->drop_offer_status = 3;  // auto-decline -> NPC eligible
                std::cout << "[Arbiter] Drop offer timeout (5s). Auto-declined; NPC eligible.\n";
            }
            sem_post(&state->state_lock);
            break;
        }
        usleep(15 * 1000);
    }
    if (state->quit_requested) {
        sem_wait(&state->state_lock);
        if (state->drop_offer_status == 1 && state->drop_offer_weapon_id > 0) {
            state->drop_offer_status = 3;
        }
        sem_post(&state->state_lock);
    }

    sem_wait(&state->state_lock);
    const int outcome = state->drop_offer_status;
    const int wid = state->drop_offer_weapon_id;
    if (outcome == 2 && wid > 0) {
        const int player_slot = 0;
        const int wsz = RequiredSlotsForWeapon(wid);
        EnsureWeaponPlacementInInventory(state, player_slot, wid, wsz);
        CompactPlayerMemory(state, player_slot);
        state->drop_offer_weapon_id = 0;
        state->drop_offer_status = 0;
    } else if (outcome == 3 && wid > 0) {
        PushDroppedWeapon(state, wid);
        state->drop_offer_weapon_id = 0;
        state->drop_offer_status = 0;
    }
    sem_post(&state->state_lock);
}
}  // namespace

int main() {
    SetupSignalHandlers();
    shm_unlink(kSharedMemoryName);

    int fd = shm_open(kSharedMemoryName, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        Fail("shm_open failed");
    }

    if (ftruncate(fd, sizeof(SharedState)) == -1) {
        Fail("ftruncate failed");
    }

    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        Fail("mmap failed");
    }

    auto* state = static_cast<SharedState*>(memory);
    std::memset(state, 0, sizeof(SharedState));

    if (sem_init(&state->state_lock, 1, 1) == -1) Fail("sem_init(state_lock) failed");
    if (sem_init(&state->turn_signal_hip, 1, 0) == -1) Fail("sem_init(turn_signal_hip) failed");
    if (sem_init(&state->turn_signal_asp, 1, 0) == -1) Fail("sem_init(turn_signal_asp) failed");
    if (sem_init(&state->action_ready, 1, 0) == -1) Fail("sem_init(action_ready) failed");

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(&state->turn_lock, &mattr) != 0) Fail("pthread_mutex_init(turn_lock) failed");
    for (int i = 0; i < 2; ++i) {
        if (pthread_mutex_init(&state->humans[i].input_lock, &mattr) != 0) Fail("pthread_mutex_init(input_lock) failed");
        if (sem_init(&state->humans[i].turn_ready, 1, 0) == -1) Fail("sem_init(turn_ready) failed");
    }
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    if (pthread_cond_init(&state->turn_changed, &cattr) != 0) Fail("pthread_cond_init(turn_changed) failed");
    pthread_condattr_destroy(&cattr);

    const char* env_mode = getenv("GAME_MODE");
    if (env_mode) {
        if (std::string(env_mode) == "pvp") state->game_mode = 1;
        else if (std::string(env_mode) == "coop") state->game_mode = 2;
        else state->game_mode = 0;
    } else {
        state->game_mode = 0;
    }

    StartStunResumeWorker();

    int configured_roll = ParseEnvInt("ROLL_NO", kDefaultRollNumber);
    if (configured_roll < 0) configured_roll = kDefaultRollNumber;
    int configured_party_size = 1;
    int auto_party_size = ParseEnvInt("AUTO_PARTY_SIZE", 0);

    std::mt19937 rng(static_cast<unsigned>(configured_roll));
    g_rng_ptr = &rng;
    std::uniform_int_distribution<int> player_hp_bonus(100, 1000);
    std::uniform_int_distribution<int> enemy_hp_bonus(50, 200);
    std::uniform_int_distribution<int> enemy_speed_dist(10, 30);

    state->roll_number_seed = configured_roll;
    state->player_count = configured_party_size;
    std::srand(static_cast<unsigned>(configured_roll));
    state->enemy_count = 2 + (std::rand() % 8);
    state->players_alive = state->player_count;
    state->enemies_alive = state->enemy_count;
    state->total_enemy_kills = 0;
    state->arbiter_pid = static_cast<int>(getpid());
    state->hip_pid = -1;
    state->asp_pid = -1;
    state->quit_requested = 0;
    state->setup_phase = 1;
    state->setup_party_size = 1;
    if (auto_party_size >= 1 && auto_party_size <= kMaxPlayers) {
        state->setup_phase = 0;
        state->setup_party_size = auto_party_size;
        std::cout << "[Arbiter] AUTO_PARTY_SIZE enabled: " << auto_party_size << "\n";
    }
    for (int i = 0; i < 300; ++i) state->actor_waiting_artifact[i] = -1;
    std::cout << "[Arbiter] Waiting for GUI menu setup (party size)...\n";
    while (!g_terminate_requested) {
        sem_wait(&state->state_lock);
        const int ready = state->setup_phase == 0;
        const int wants_quit = state->quit_requested;
        sem_post(&state->state_lock);
        if (ready) break;
        if (wants_quit) {
            state->game_running = 0;
            sem_post(&state->turn_signal_hip);
            sem_post(&state->turn_signal_asp);
            std::cout << "[Arbiter] Quit requested during setup.\n";
            StopStunResumeWorker();
            SaveStorageState(state);
            sem_destroy(&state->state_lock);
            sem_destroy(&state->turn_signal_hip);
            sem_destroy(&state->turn_signal_asp);
            sem_destroy(&state->action_ready);
            pthread_mutex_destroy(&state->turn_lock);
            for (int i = 0; i < 2; ++i) {
                pthread_mutex_destroy(&state->humans[i].input_lock);
                sem_destroy(&state->humans[i].turn_ready);
            }
            pthread_cond_destroy(&state->turn_changed);
            munmap(memory, sizeof(SharedState));
            close(fd);
            shm_unlink(kSharedMemoryName);
            return 0;
        }
        usleep(100 * 1000);
    }
    sem_wait(&state->state_lock);
    configured_party_size = state->setup_party_size;
    sem_post(&state->state_lock);
    if (configured_party_size < 1) configured_party_size = 1;
    if (configured_party_size > kMaxPlayers) configured_party_size = kMaxPlayers;

    state->player_count = configured_party_size;
    state->players_alive = state->player_count;
    state->artifact_present[0] = 1;
    state->artifact_present[1] = 1;
    state->artifact_present[2] = 0;
    state->artifact_holder[0] = -1;
    state->artifact_holder[1] = -1;
    state->artifact_holder[2] = -1;
    state->eclipse_spawned = 0;
    state->dropped_weapon_count = 0;

    const int roll_no = configured_roll;
    const int last_digit = roll_no % 10;
    const int second_last = (roll_no / 10) % 10;
    const int last_two = roll_no % 100;

    for (int i = 0; i < state->player_count; ++i) {
        state->player_alive[i] = 1;
        state->player_hp[i] = roll_no + player_hp_bonus(rng);
        state->player_max_hp[i] = state->player_hp[i];
        state->player_damage[i] = last_digit + 10;
        state->player_speed[i] = 100 / state->player_count;
        state->player_stamina[i] = 0;
        state->player_max_stamina[i] = 100;
    }

    for (int i = 0; i < state->enemy_count; ++i) {
        state->enemy_alive[i] = 1;
        state->enemy_hp[i] = last_two + enemy_hp_bonus(rng);
        state->enemy_max_hp[i] = state->enemy_hp[i];
        state->enemy_damage[i] = second_last + 10;
        state->enemy_speed[i] = enemy_speed_dist(rng);
        state->enemy_stamina[i] = 0;
        state->enemy_max_stamina[i] = 150;
    }

    for (int i = 0; i < state->player_count; ++i) {
        for (int s = 0; s < kStorageSlots; ++s) {
            state->storage_slots[i][s] = 0;
            state->storage_block_size[i][s] = 0;
        }
        for (int inv = 0; inv < kInventorySlots; ++inv) {
            state->inventory_slots[i][inv] = 0;
            state->inventory_block_size[i][inv] = 0;
        }
        PlaceInStorage(state, i, 8);
        RefreshPlayerMemoryLayout(state, i);
    }
    if (state->player_count > 0) {
        PlaceInFirstContiguousFree(state->inventory_slots[0], state->inventory_block_size[0], kInventorySlots, 1,
                                   RequiredSlotsForWeapon(1));  // Solar Core
        PlaceInFirstContiguousFree(state->inventory_slots[0], state->inventory_block_size[0], kInventorySlots, 2,
                                   RequiredSlotsForWeapon(2));  // Lunar Blade
        RefreshPlayerMemoryLayout(state, 0);
    }

    LoadStorageState(state);

    state->current_turn = 1;
    state->game_running = 1;
    state->acting_side = 0;
    state->acting_id = 100;

    std::cout << "[Arbiter] Initialized shared state.\n";
    std::cout << "[Arbiter] Roll seed: " << state->roll_number_seed << "\n";
    std::cout << "[Arbiter] Players: " << state->player_count << ", Enemies: " << state->enemy_count << "\n";
    std::cout << "[Arbiter] Enemy count is randomized by design (2-9).\n";

    std::atomic<bool> monitor_running{true};
    std::thread deadlock_monitor([&] {
        while (monitor_running.load()) {
            usleep(200 * 1000);
            sem_wait(&state->state_lock);
            /* Walk the wait-for graph from each waiter; if we re-enter a node
             * we have already visited along this path, that prefix forms a
             * cycle. With three artifacts (Solar/Lunar/Eclipse) the longest
             * possible cycle is 3, so a fixed-depth walk is sufficient.
             * Resolution: release artifacts held by the highest-id actor
             * inside the cycle (deterministic + works for both player/NPC). */
            int detected_victim = -1;
            int detected_len = 0;
            for (int seed_actor = 0; seed_actor < 300 && detected_victim == -1; ++seed_actor) {
                if (state->actor_waiting_artifact[seed_actor] < 0) continue;
                int path[5];
                int path_len = 0;
                int cur = seed_actor;
                while (path_len < 5) {
                    int already_at = -1;
                    for (int p = 0; p < path_len; ++p) {
                        if (path[p] == cur) { already_at = p; break; }
                    }
                    if (already_at >= 0) {
                        int victim = path[already_at];
                        for (int p = already_at + 1; p < path_len; ++p) {
                            if (path[p] > victim) victim = path[p];
                        }
                        detected_victim = victim;
                        detected_len = path_len - already_at;
                        break;
                    }
                    path[path_len++] = cur;
                    const int wait_art = state->actor_waiting_artifact[cur];
                    if (wait_art < 0 || wait_art > 2) break;
                    const int next_holder = state->artifact_holder[wait_art];
                    if (next_holder < 0 || next_holder >= 300) break;
                    cur = next_holder;
                }
            }
            if (detected_victim >= 0) {
                ReleaseActorArtifacts(state, detected_victim);
                std::cout << "[Arbiter] Deadlock detected (cycle len " << detected_len
                          << "). Forced release from actor " << detected_victim << "\n";
            }
            sem_post(&state->state_lock);
        }
    });

    while (!g_terminate_requested && !state->quit_requested && state->players_alive > 0 &&
           state->total_enemy_kills < 10) {
        if (g_resume_asp_requested && g_asp_frozen_for_ultimate && state->asp_pid > 0) {
            SafeKill(state->asp_pid, SIGCONT);
            g_asp_frozen_for_ultimate = false;
            for (int i = 0; i < state->enemy_count; ++i) {
                if (state->enemy_alive[i]) {
                    state->enemy_stamina[i] =
                        std::min(state->enemy_max_stamina[i], state->enemy_stamina[i] + state->enemy_speed[i] * 2);
                }
            }
            g_resume_asp_requested = 0;
        }

        if (g_reaped_count > 0) {
            sem_wait(&state->state_lock);
            for (int i = 0; i < g_reaped_count; ++i) {
                pid_t p = g_reaped_pids[i];
                for (int h = 0; h < 2; ++h) {
                    if (state->humans[h].pid == p && state->humans[h].connected) {
                        state->humans[h].connected = 0;
                        std::cout << "[Arbiter] Player " << (h+1) << " disconnected (PID " << p << "). Forfeiting party.\n";
                        for (int c = 0; c < state->player_count; ++c) {
                            if ((state->game_mode == 0) || (c % 2 == h)) {
                                state->player_hp[c] = 0;
                                state->player_alive[c] = 0;
                                --state->players_alive;
                            }
                        }
                    }
                }
            }
            g_reaped_count = 0;
            sem_post(&state->state_lock);
        }

        // Respawn enemies if all dead but win condition not yet met.
        if (state->enemies_alive <= 0 && state->total_enemy_kills < 10) {
            const int new_count = std::min(kMaxEnemies, 10 - state->total_enemy_kills);
            state->enemy_count = std::max(state->enemy_count, new_count);
            state->enemies_alive = 0;
            for (int i = 0; i < state->enemy_count && state->enemies_alive < new_count; ++i) {
                if (!state->enemy_alive[i]) {
                    state->enemy_alive[i] = 1;
                    state->enemy_hp[i] = (configured_roll % 100) + enemy_hp_bonus(rng);
                    state->enemy_max_hp[i] = state->enemy_hp[i];
                    state->enemy_damage[i] = ((configured_roll / 10) % 10) + 10;
                    state->enemy_speed[i] = enemy_speed_dist(rng);
                    state->enemy_stamina[i] = 0;
                    state->enemy_max_stamina[i] = 150;
                    state->enemy_stunned[i] = 0;
                    state->enemy_stun_until[i] = 0;
                    state->enemy_carrying_weapon[i] = 0;
                    ++state->enemies_alive;
                }
            }
            std::cout << "[Arbiter] Respawned " << state->enemies_alive << " enemies (kills so far: " << state->total_enemy_kills << ").\n";
        }

        if (!state->eclipse_spawned && state->total_enemy_kills >= 3) {
            state->artifact_present[2] = 1;
            state->artifact_holder[2] = -1;
            state->eclipse_spawned = 1;
            std::cout << "[Arbiter] Eclipse Relic has appeared.\n";
        }

        // Arrival-time scheduling with deterministic tie-break:
        // choose actor with minimum ticks-to-ready, then lowest actor_id.
        while (true) {
            int best_ticks = INT_MAX;
            int best_actor = -1;
            int best_side = -1;

            for (int i = 0; i < state->player_count; ++i) {
                if (!state->player_alive[i]) continue;
                state->player_stunned[i] = IsStunnedUntil(state->player_stun_until[i]) ? 1 : 0;
                if (state->player_stunned[i]) continue;
                const int speed = std::max(1, state->player_speed[i]);
                const int remaining = std::max(0, state->player_max_stamina[i] - state->player_stamina[i]);
                const int ticks = (remaining + speed - 1) / speed;
                const int actor_id = 100 + i;
                if (ticks < best_ticks || (ticks == best_ticks && actor_id < best_actor)) {
                    best_ticks = ticks;
                    best_actor = actor_id;
                    best_side = 0;
                }
            }

            for (int i = 0; i < state->enemy_count; ++i) {
                if (!state->enemy_alive[i]) continue;
                state->enemy_stunned[i] = IsStunnedUntil(state->enemy_stun_until[i]) ? 1 : 0;
                if (state->enemy_stunned[i]) continue;
                if (g_asp_frozen_for_ultimate) continue;
                const int speed = std::max(1, state->enemy_speed[i]);
                const int remaining = std::max(0, state->enemy_max_stamina[i] - state->enemy_stamina[i]);
                const int ticks = (remaining + speed - 1) / speed;
                const int actor_id = 200 + i;
                if (ticks < best_ticks || (ticks == best_ticks && actor_id < best_actor)) {
                    best_ticks = ticks;
                    best_actor = actor_id;
                    best_side = 1;
                }
            }

            if (best_actor != -1) {
                for (int i = 0; i < state->player_count; ++i) {
                    if (!state->player_alive[i] || state->player_stunned[i]) continue;
                    state->player_stamina[i] = std::min(state->player_max_stamina[i],
                                                        state->player_stamina[i] + state->player_speed[i] * best_ticks);
                }
                for (int i = 0; i < state->enemy_count; ++i) {
                    if (!state->enemy_alive[i] || state->enemy_stunned[i] || g_asp_frozen_for_ultimate) continue;
                    state->enemy_stamina[i] = std::min(state->enemy_max_stamina[i],
                                                       state->enemy_stamina[i] + state->enemy_speed[i] * best_ticks);
                }

                state->acting_side = best_side;
                state->acting_id = best_actor;
                if (state->current_turn <= 5 || (state->current_turn % 5) == 0) {
                    std::cout << "[Arbiter][Arrival] turn=" << state->current_turn << " actor=" << best_actor
                              << " eta_ticks=" << best_ticks << " tie_break=lowest_actor_id\n";
                }
                break;
            }
            int tick_ms = 100;
            if (const char* env_tick = getenv("TICK_RATE_MS")) {
                tick_ms = std::stoi(env_tick);
            }
            usleep(tick_ms * 1000);  // ATB pacing
        }

        std::cout << "[Arbiter] Turn " << state->current_turn << " actor " << state->acting_id << "\n";
        if (state->acting_side == 0) {
            pthread_mutex_lock(&state->turn_lock);
            pthread_cond_broadcast(&state->turn_changed);
            pthread_mutex_unlock(&state->turn_lock);
        }
        if (state->acting_side == 1) sem_post(&state->turn_signal_asp);

        bool action_received = true;
        if (state->acting_side == 1) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            if (sem_timedwait(&state->action_ready, &deadline) == -1) {
                if (errno == ETIMEDOUT) {
                    action_received = false;
                } else {
                    Fail("sem_timedwait(action_ready) failed");
                }
            }
        } else {
            sem_wait(&state->action_ready);
        }

        sem_wait(&state->state_lock);
        ActionPacket action{};
        if (action_received) {
            action = state->pending_action;
        } else {
            action.actor_id = state->acting_id;
            action.action = ActionType::Skip;
            action.target_id = -1;
            action.value = 0;
            action.weapon_id = 0;
            action.aux = 0;
            std::cout << "[Arbiter] ASP timeout (3s). Auto-applying Skip for actor " << action.actor_id << "\n";
        }
        ApplyAction(state, action);
        if (action.actor_id >= 100 && action.actor_id < 200) {
            int idx = action.actor_id - 100;
            if (idx >= 0 && idx < state->player_count) {
                if (action.action == ActionType::Skip) {
                    state->player_stamina[idx] = state->player_max_stamina[idx] / 2;
                } else {
                    state->player_stamina[idx] = 0;
                }
            }
        } else if (action.actor_id >= 200 && action.actor_id < 300) {
            int idx = action.actor_id - 200;
            if (idx >= 0 && idx < state->enemy_count) {
                if (action.action == ActionType::Skip) {
                    state->enemy_stamina[idx] = state->enemy_max_stamina[idx] / 2;
                } else {
                    state->enemy_stamina[idx] = 0;
                }
            }
        }
        sem_post(&state->state_lock);

        PumpDropOfferHandshake(state);

        std::cout << "[Arbiter] Action from actor " << action.actor_id << " type "
                  << static_cast<int>(action.action) << " target " << action.target_id
                  << " value " << action.value << "\n";
        std::cout << "[Arbiter] Status: players_alive=" << state->players_alive
                  << " enemies_alive=" << state->enemies_alive
                  << " total_kills=" << state->total_enemy_kills << "\n";
        ++state->current_turn;
    }

    state->game_running = 0;
    monitor_running.store(false);
    if (deadlock_monitor.joinable()) deadlock_monitor.join();
    pthread_mutex_lock(&state->turn_lock);
    pthread_cond_broadcast(&state->turn_changed);
    pthread_mutex_unlock(&state->turn_lock);
    sem_post(&state->turn_signal_asp);

    if (state->quit_requested || g_terminate_requested) {
        state->game_over_reason = 3;
        std::cout << "[Arbiter] Quit condition reached.\n";
    } else if (state->players_alive <= 0) {
        state->game_over_reason = 2;
        std::cout << "[Arbiter] Lose condition reached: all players dead.\n";
    } else if (state->total_enemy_kills >= 10) {
        state->game_over_reason = 1;
        std::cout << "[Arbiter] Win condition reached: 10 enemies defeated.\n";
    } else {
        std::cout << "[Arbiter] Game loop finished.\n";
    }

    SaveStorageState(state);
    StopStunResumeWorker();
    sem_destroy(&state->state_lock);
    sem_destroy(&state->turn_signal_hip);
    sem_destroy(&state->turn_signal_asp);
    sem_destroy(&state->action_ready);
    pthread_mutex_destroy(&state->turn_lock);
    for (int i = 0; i < 2; ++i) {
        pthread_mutex_destroy(&state->humans[i].input_lock);
        sem_destroy(&state->humans[i].turn_ready);
    }
    pthread_cond_destroy(&state->turn_changed);
    munmap(memory, sizeof(SharedState));
    close(fd);
    shm_unlink(kSharedMemoryName);
    return 0;
}
