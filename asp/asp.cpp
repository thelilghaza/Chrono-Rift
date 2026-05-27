#include "../common/game_shared.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <random>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void Fail(const char* msg) {
    std::cerr << msg << ": " << std::strerror(errno) << "\n";
    std::exit(1);
}

void OnSigUsr1(int) {}

void InstallStunSignalHooks() {
    struct sigaction sa {};
    sa.sa_handler = OnSigUsr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);
}
}  // namespace

int main() {
    int fd = -1;
    for (int attempts = 0; attempts < 50; ++attempts) {
        fd = shm_open(kSharedMemoryName, O_RDWR, 0666);
        if (fd != -1) break;
        usleep(100 * 1000);
    }
    if (fd == -1) Fail("ASP shm_open failed");

    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        Fail("ASP mmap failed");
    }

    auto* state = static_cast<SharedState*>(memory);
    InstallStunSignalHooks();
    sem_wait(&state->state_lock);
    state->asp_pid = static_cast<int>(getpid());
    sem_post(&state->state_lock);
    std::cout << "[ASP] Connected to shared memory.\n";

    /* Wait until Arbiter has set enemy_count and player_count; protects
     * against attach happening before initialization completes. */
    int enemy_count_local = 0;
    while (true) {
        sem_wait(&state->state_lock);
        const bool ready = state->enemy_count > 0 && state->setup_phase == 0 && state->player_count > 0;
        const bool stop = state->quit_requested != 0;
        if (ready) enemy_count_local = state->enemy_count;
        sem_post(&state->state_lock);
        if (ready || stop) break;
        usleep(50 * 1000);
    }
    if (enemy_count_local <= 0) {
        std::cout << "[ASP] Quit before enemy_count known. Exiting.\n";
        munmap(memory, sizeof(SharedState));
        close(fd);
        return 0;
    }

    std::mutex local_mutex;
    std::condition_variable cv;
    bool shutdown = false;
    int scheduled_actor = -1;
    bool turn_open = false;

    auto npc_worker = [&](int enemy_idx) {
        const int actor_id = 200 + enemy_idx;
        const unsigned seed_core = static_cast<unsigned>(std::max(0, state->roll_number_seed));
        std::mt19937 rng(seed_core ^ (static_cast<unsigned>(enemy_idx + 1u) * 0x9E3779B9u));
        std::uniform_int_distribution<int> roll(0, 99);
        while (true) {
            std::unique_lock<std::mutex> lk(local_mutex);
            cv.wait(lk, [&] { return shutdown || (turn_open && scheduled_actor == actor_id); });
            if (shutdown) return;
            turn_open = false;
            lk.unlock();

            sem_wait(&state->state_lock);
            int target_id = -1;
            int alive_targets[kMaxPlayers];
            int alive_count = 0;
            for (int i = 0; i < state->player_count; ++i) {
                if (state->player_alive[i]) {
                    alive_targets[alive_count++] = 100 + i;
                }
            }
            if (alive_count > 0) {
                std::uniform_int_distribution<int> target_pick(0, alive_count - 1);
                target_id = alive_targets[target_pick(rng)];
            }
            /* Spec §6/§10: when a player declines a drop, an enemy is
             * guaranteed to claim it. We honor the pickup rule by removing
             * the dropped weapon from the shared pool and tagging this NPC
             * as carrying it. NPCs do NOT use weapon damage themselves --
             * spec §10 limits enemy actions to Strike or Skip -- so the
             * weapon simply leaves the pool (and prevents a death-drop). */
            if (state->dropped_weapon_count > 0 && state->enemy_carrying_weapon[enemy_idx] == 0 &&
                roll(rng) < 50) {
                state->enemy_carrying_weapon[enemy_idx] =
                    state->dropped_weapon_ids[state->dropped_weapon_count - 1];
                --state->dropped_weapon_count;
            }
            ActionPacket action{};
            action.actor_id = actor_id;
            action.action = ActionType::Strike;
            action.target_id = target_id;
            action.value = state->enemy_damage[enemy_idx];
            action.weapon_id = 0;
            action.aux = 0;
            if (roll(rng) < 20) {
                action.action = ActionType::Skip;
                action.target_id = -1;
                action.value = 0;
            }
            state->pending_action = action;
            sem_post(&state->state_lock);
            sem_post(&state->action_ready);
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < enemy_count_local; ++i) {
        workers.emplace_back(npc_worker, i);
    }

    while (true) {
        sem_wait(&state->turn_signal_asp);
        if (!state->game_running) {
            std::lock_guard<std::mutex> lk(local_mutex);
            shutdown = true;
            cv.notify_all();
            break;
        }

        {
            std::lock_guard<std::mutex> lk(local_mutex);
            scheduled_actor = state->acting_id;
            turn_open = true;
        }
        cv.notify_all();
    }

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    std::cout << "[ASP] Stopping.\n";
    munmap(memory, sizeof(SharedState));
    close(fd);
    return 0;
}
