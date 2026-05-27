#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <semaphore.h>

constexpr int kMaxPlayers = 4;
constexpr int kMaxEnemies = 10;
constexpr int kInventorySlots = 20;
constexpr int kStorageSlots = 20;
constexpr int kDefaultRollNumber = 932;
constexpr const char* kSharedMemoryName = "/chrono_rift_state";
constexpr int kMaxDroppedWeapons = 32;

enum class ActionType : int {
    Skip = 0,
    Strike = 1,
    Exhaust = 2,
    Heal = 3,
    UseWeapon = 4,
    SwapIn = 5,
    Ultimate = 6,
};

struct ActionPacket {
    int actor_id = -1;
    ActionType action = ActionType::Skip;
    int target_id = -1;
    int value = 0;
    int weapon_id = 0;
    int aux = 0;
};

struct HumanState {
    int pid = -1;
    int connected = 0;
    pthread_mutex_t input_lock{};
    sem_t turn_ready{};
};

struct SharedState {
    sem_t state_lock{};
    sem_t turn_signal_hip{};
    sem_t turn_signal_asp{};
    sem_t action_ready{};
    pthread_mutex_t turn_lock{};
    pthread_cond_t turn_changed{};
    HumanState humans[2]{};

    int game_mode = 0;
    int game_running = 0;
    int game_over_reason = 0;
    int roll_number_seed = 0;
    int setup_phase = 1;
    int setup_party_size = 1;
    int quit_requested = 0;
    int current_turn = 0;
    int acting_side = 0;
    int acting_id = -1;
    int player_count = 0;
    int enemy_count = 0;
    int players_alive = 0;
    int enemies_alive = 0;
    int total_enemy_kills = 0;
    int arbiter_pid = -1;
    int hip_pid = -1;
    int asp_pid = -1;
    int eclipse_spawned = 0;
    int dropped_weapon_count = 0;
    int drop_offer_status = 0;
    int drop_offer_weapon_id = 0;

    std::array<int, kMaxPlayers> player_alive{};
    std::array<int, kMaxPlayers> player_hp{};
    std::array<int, kMaxPlayers> player_max_hp{};
    std::array<int, kMaxPlayers> player_damage{};
    std::array<int, kMaxPlayers> player_speed{};
    std::array<int, kMaxPlayers> player_stamina{};
    std::array<int, kMaxPlayers> player_max_stamina{};
    std::array<int, kMaxPlayers> player_stunned{};
    std::array<int, kMaxPlayers> player_stun_until{};
    std::array<int, kMaxPlayers> inventory_fragments{};
    std::array<int, kMaxPlayers> storage_fragments{};
    std::array<int, kMaxPlayers * kInventorySlots> inventory_flat{};
    std::array<int, kMaxPlayers * kStorageSlots> storage_flat{};
    int inventory_slots[kMaxPlayers][kInventorySlots]{};
    int inventory_block_size[kMaxPlayers][kInventorySlots]{};
    int storage_slots[kMaxPlayers][kStorageSlots]{};
    int storage_block_size[kMaxPlayers][kStorageSlots]{};

    std::array<int, kMaxEnemies> enemy_alive{};
    std::array<int, kMaxEnemies> enemy_hp{};
    std::array<int, kMaxEnemies> enemy_max_hp{};
    std::array<int, kMaxEnemies> enemy_damage{};
    std::array<int, kMaxEnemies> enemy_speed{};
    std::array<int, kMaxEnemies> enemy_stamina{};
    std::array<int, kMaxEnemies> enemy_max_stamina{};
    std::array<int, kMaxEnemies> enemy_stunned{};
    std::array<int, kMaxEnemies> enemy_stun_until{};
    std::array<int, kMaxEnemies> enemy_carrying_weapon{};

    std::array<int, 3> artifact_present{};
    std::array<int, 3> artifact_holder{};
    std::array<int, 300> actor_waiting_artifact{};
    std::array<int, kMaxDroppedWeapons> dropped_weapon_ids{};

    ActionPacket pending_action{};
    ActionPacket gui_action_packet{};
    int gui_action_pending = 0;
};
