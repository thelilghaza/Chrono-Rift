#include "../common/game_shared.h"

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <csignal>
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

void OnSigUsr1(int) {
}

void InstallStunSignalHooks() {
    struct sigaction sa {};
    sa.sa_handler = OnSigUsr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);
}
}  // namespace

int main(int argc, char** argv) {
    int player_id = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--player-id" && i + 1 < argc) {
            player_id = std::stoi(argv[i + 1]);
        }
    }

    int fd = -1;
    for (int attempts = 0; attempts < 50; ++attempts) {
        fd = shm_open(kSharedMemoryName, O_RDWR, 0666);
        if (fd != -1) break;
        usleep(100 * 1000);
    }
    if (fd == -1) Fail("HIP shm_open failed");

    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        Fail("HIP mmap failed");
    }

    auto* state = static_cast<SharedState*>(memory);
    InstallStunSignalHooks();
    
    sem_wait(&state->state_lock);
    int game_mode = state->game_mode;
    if (player_id == 0 || player_id == 1) {
        state->humans[0].pid = getpid();
        state->humans[0].connected = 1;
        state->hip_pid = getpid();
    }
    if (player_id == 2) {
        state->humans[1].pid = getpid();
        state->humans[1].connected = 1;
    }
    sem_post(&state->state_lock);
    
    std::cout << "[HIP] Connected to shared memory. Mode: " << game_mode << " PlayerID: " << player_id << "\n";

    if (game_mode == 0) player_id = 0;

    /* Spec §2: "A separate thread must be created for each player-controlled
     * character." Defer spawning workers until the GUI menu has set the
     * party size and the Arbiter has propagated it to player_count, so we
     * spawn exactly party_size threads (not kMaxPlayers placeholders). */
    int actual_party_size = 0;
    while (true) {
        sem_wait(&state->state_lock);
        const bool ready = state->setup_phase == 0 && state->player_count > 0;
        const bool stop = state->quit_requested != 0;
        if (ready) actual_party_size = state->player_count;
        sem_post(&state->state_lock);
        if (ready || stop) break;
        usleep(50 * 1000);
    }
    if (actual_party_size <= 0) {
        std::cout << "[HIP " << player_id << "] Quit before party size known. Exiting.\n";
        munmap(memory, sizeof(SharedState));
        close(fd);
        return 0;
    }

    std::mutex local_mutex;
    std::condition_variable cv;
    bool shutdown = false;
    int scheduled_actor = -1;
    bool turn_open = false;

    auto player_worker = [&](int player_idx) {
        const int actor_id = 100 + player_idx;
        while (true) {
            std::unique_lock<std::mutex> lk(local_mutex);
            cv.wait(lk, [&] { return shutdown || (turn_open && scheduled_actor == actor_id); });
            if (shutdown) return;
            turn_open = false;
            lk.unlock();

            ActionPacket action{};
            action.actor_id = actor_id;
            action.action = ActionType::Skip;
            action.target_id = -1;
            action.value = 0;
            action.weapon_id = 0;
            action.aux = 0;

            bool has_gui_action = false;
            for (int wait_ticks = 0; wait_ticks < 600; ++wait_ticks) {
                sem_wait(&state->state_lock);
                if (!state->game_running) {
                    sem_post(&state->state_lock);
                    return;
                }
                if (state->gui_action_pending && state->gui_action_packet.actor_id == actor_id) {
                    action = state->gui_action_packet;
                    state->gui_action_pending = 0;
                    has_gui_action = true;
                }
                sem_post(&state->state_lock);
                if (has_gui_action) break;
                usleep(50 * 1000);
            }

            if (!has_gui_action) {
                sem_wait(&state->state_lock);
                const int damage = state->player_damage[player_idx];
                int fallback_target = -1;
                for (int i = 0; i < state->enemy_count; ++i) {
                    if (state->enemy_alive[i]) {
                        fallback_target = 200 + i;
                        break;
                    }
                }
                action.action = ActionType::Strike;
                action.target_id = fallback_target;
                action.value = damage;
                sem_post(&state->state_lock);
            }

            sem_wait(&state->state_lock);
            state->pending_action = action;
            sem_post(&state->state_lock);
            sem_post(&state->action_ready);
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < actual_party_size; ++i) {
        if (player_id == 0 || (i % 2) == (player_id - 1)) {
            workers.emplace_back(player_worker, i);
        }
    }

    int last_processed_turn = -1;
    while (true) {
        pthread_mutex_lock(&state->turn_lock);
        while (!state->quit_requested && !state->game_over_reason && 
               (!state->game_running || state->acting_side != 0 || state->current_turn == last_processed_turn)) {
            pthread_cond_wait(&state->turn_changed, &state->turn_lock);
        }
        
        if (!state->game_running) {
            pthread_mutex_unlock(&state->turn_lock);
            std::lock_guard<std::mutex> lk(local_mutex);
            shutdown = true;
            cv.notify_all();
            break;
        }

        int current_turn = state->current_turn;
        int actor_idx = state->acting_id - 100;
        bool we_own_it = (player_id == 0) || (actor_idx % 2 == (player_id - 1));

        if (we_own_it) {
            last_processed_turn = current_turn;
            std::lock_guard<std::mutex> lk(local_mutex);
            scheduled_actor = state->acting_id;
            turn_open = true;
            cv.notify_all();
        }
        pthread_mutex_unlock(&state->turn_lock);
    }

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    std::cout << "[HIP " << player_id << "] Stopping.\n";
    munmap(memory, sizeof(SharedState));
    close(fd);
    return 0;
}
