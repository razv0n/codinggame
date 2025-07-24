#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <map>
#include <set>
#include <climits>
#include <queue>
#include <random>
#include <unordered_map>
#include <stdexcept>
#include <memory>
#include <limits>
#include <fstream>
using namespace std;

const bool WETNESS_AFFECTS_DISTANCE = true;
const bool COLLISIONS = true;
const int THROW_DAMAGE = 30;
const int THROW_DISTANCE_MAX = 4;

enum class GameAgentClass {
    GUNNER,
    SNIPER,
    BOMBER,
    ASSAULT,
    BERSERKER
};

struct GameMechanics {
    static int calculate_exact_shooting_damage(int soaking_power, int optimal_range, int distance) {
        if (distance > optimal_range || distance == 0) return 0;
        
        int base_damage = soaking_power;
        
        
        if (distance > 1) {
            double penalty = 0.25 * (distance - 1);
            base_damage = (int)(base_damage * (1.0 - penalty));
        }
        
        return max(0, base_damage);
    }
    
    
    static int calculate_exact_bomb_damage(int splash_distance, bool is_hunkered) {
        if (splash_distance > 1) return 0; 
        
        int damage = THROW_DAMAGE;
        if (is_hunkered) {
            damage = damage / 2; 
        }
        
        return damage;
    }
    
    
    static int calculate_movement_cost(int wetness) {
        if (!WETNESS_AFFECTS_DISTANCE) return 1;
        
        double wetness_factor = 1.0 + (wetness * 0.01); 
        return (int)ceil(wetness_factor);
    }
    
    
    static bool is_valid_movement_position(int x, int y, int board_width, int board_height, 
                                         const vector<vector<int>>& tile_map,
                                         const vector<pair<int, int>>& occupied_positions) {
        
        if (x < 0 || x >= board_width || y < 0 || y >= board_height) return false;
        
        
        for (const auto& pos : occupied_positions) {
            if (pos.first == x && pos.second == y) return false;
        }
        
        
        
        return true;
    }
    
    
    static bool provides_cover(int x, int y, int board_width, int board_height, 
                              const vector<vector<int>>& tile_map) {
        if (x < 0 || x >= board_width || y < 0 || y >= board_height) return false;
        if (y >= 0 && y < tile_map.size() && x >= 0 && x < tile_map[0].size()) {
            return tile_map[y][x] == 1; 
        }
        return false;
    }
    
    
    static double calculate_kill_probability(int current_wetness, int damage) {
        if (current_wetness + damage >= 100) return 1.0;
        return (double)(current_wetness + damage) / 100.0;
    }
    
    
    static double calculate_tactical_advantage(int my_agents_alive, int enemy_agents_alive, 
                                              int my_total_health, int enemy_total_health) {
        double agent_ratio = (double)my_agents_alive / max(1, enemy_agents_alive);
        double health_ratio = (double)my_total_health / max(1, enemy_total_health);
        
        return (agent_ratio * 0.6) + (health_ratio * 0.4); 
    }
};


struct SmartGameAI {
    struct AgentData {
        int agent_id;
        int player;
        int shoot_cooldown;
        int optimal_range;
        int soaking_power;
        int splash_bombs;
        GameAgentClass agent_class;
    };

    struct AgentState {
        int agent_id;
        int x, y;
        int cooldown;
        int splash_bombs;
        int wetness;
        bool is_alive() const { return wetness < 100; }
        int get_health() const { return 100 - wetness; }
    };

    struct TacticalDecision {
        string action_type;
        int target_x = -1, target_y = -1;
        int target_agent_id = -1;
        int bomb_x = -1, bomb_y = -1;  
        double expected_value = 0.0;
        double kill_probability = 0.0;
        int expected_damage = 0;
        string tactical_reasoning = "";
    };

    unordered_map<int, AgentData> all_agents_data;
    vector<int> my_agent_ids;
    vector<int> enemy_agent_ids;
    int board_width, board_height;
    vector<vector<int>> tile_map;

    struct GameSimulator {
        string game_folder_path;
        int simulation_id;
        
        GameSimulator() : game_folder_path("./game/"), simulation_id(0) {}
        
        void save_game_state(const vector<AgentState>& my_agents, const vector<AgentState>& enemies,
                           int board_width, int board_height, const vector<vector<int>>& tile_map) {
            
            string state_file = game_folder_path + "simulation_state_" + to_string(simulation_id++) + ".txt";
            ofstream file(state_file);
            
            if (file.is_open()) {
                file << board_width << " " << board_height << "\n";
                
                for (int y = 0; y < board_height; y++) {
                    for (int x = 0; x < board_width; x++) {
                        if (y < (int)tile_map.size() && x < (int)tile_map[y].size()) {
                            file << x << " " << y << " " << tile_map[y][x] << "\n";
                        } else {
                            file << x << " " << y << " 0\n";
                        }
                    }
                }
                
                file << (my_agents.size() + enemies.size()) << "\n";
                
                for (const auto& agent : my_agents) {
                    file << agent.agent_id << " " << agent.x << " " << agent.y << " "
                         << agent.cooldown << "\n";
                }
                
                for (const auto& agent : enemies) {
                    file << agent.agent_id << " " << agent.x << " " << agent.y << " "
                         << agent.cooldown << "\n";
                }
                
                file.close();
                cerr << "📁 Game state saved to: " << state_file << endl;
            } else {
                cerr << "❌ Failed to save game state to: " << state_file << endl;
            }
        }
    };

    GameAgentClass determine_agent_class(const AgentData& data) {
        if (data.optimal_range == 6 && data.soaking_power == 24) return GameAgentClass::SNIPER;
        if (data.optimal_range == 2 && data.splash_bombs >= 3) return GameAgentClass::BOMBER;
        if (data.optimal_range == 2 && data.soaking_power == 32) return GameAgentClass::BERSERKER;
        if (data.optimal_range == 4 && data.splash_bombs >= 2) return GameAgentClass::ASSAULT;
        return GameAgentClass::GUNNER;
    }

    string get_class_name(GameAgentClass ac) {
        switch(ac) {
            case GameAgentClass::SNIPER: return "SNIPER";
            case GameAgentClass::BOMBER: return "BOMBER";
            case GameAgentClass::BERSERKER: return "BERSERKER";
            case GameAgentClass::ASSAULT: return "ASSAULT";
            default: return "GUNNER";
        }
    }

    TacticalDecision evaluate_exact_shooting(const AgentState& agent, const vector<AgentState>& enemies) {
        TacticalDecision best_shot;
        best_shot.action_type = "HUNKER_DOWN";
        best_shot.expected_value = 0;
        
        if (agent.cooldown > 0) {
            best_shot.tactical_reasoning = "Agent on cooldown";
            return best_shot;
        }
        
        if (enemies.empty()) {
            best_shot.tactical_reasoning = "No enemies visible";
            return best_shot;
        }
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        GameAgentClass agent_class = data.agent_class;
        
        cerr << "Evaluating shooting for agent " << agent.agent_id << " against " << enemies.size() << " enemies" << endl;
        
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            int damage = GameMechanics::calculate_exact_shooting_damage(data.soaking_power, data.optimal_range, distance);
            
            cerr << "  Enemy " << enemy.agent_id << " at distance " << distance << " -> damage " << damage << endl;
            
            if (damage > 0) {
                double kill_prob = GameMechanics::calculate_kill_probability(enemy.wetness, damage);
                double expected_value = damage * 100.0; 
                
                
                if (kill_prob >= 1.0) {
                    expected_value += 5000.0; 
                } else {
                    expected_value += kill_prob * 3000.0; 
                }
                
                if (agent_class == GameAgentClass::SNIPER && distance >= 4) {
                    expected_value += 2000.0; 
                }
                if (agent_class == GameAgentClass::GUNNER && distance <= 2) {
                    expected_value += 1000.0; 
                }
                if (agent_class == GameAgentClass::BERSERKER && distance <= 2) {
                    expected_value += 1500.0;
                }
                
                double wound_multiplier = 1.0 + (enemy.wetness / 100.0);
                expected_value *= wound_multiplier;
                
                cerr << "    Expected value: " << (int)expected_value << " (kill prob: " << (int)(kill_prob*100) << "%)" << endl;
                
                if (expected_value > best_shot.expected_value) {
                    best_shot.action_type = "SHOOT";
                    best_shot.target_agent_id = enemy.agent_id;
                    best_shot.expected_value = expected_value;
                    best_shot.kill_probability = kill_prob;
                    best_shot.expected_damage = damage;
                    best_shot.tactical_reasoning = "Shoot enemy " + to_string(enemy.agent_id) + 
                        " for " + to_string(damage) + " damage (kill prob: " + 
                        to_string((int)(kill_prob * 100)) + "%) at distance " + to_string(distance);
                }
            }
        }
        
        return best_shot;
    }

    TacticalDecision evaluate_exact_bombing(const AgentState& agent, const vector<AgentState>& enemies) {
        TacticalDecision best_bomb;
        best_bomb.action_type = "HUNKER_DOWN";
        best_bomb.expected_value = 0;
        
        if (agent.cooldown > 0 || agent.splash_bombs <= 0) {
            best_bomb.tactical_reasoning = "No bombs available or on cooldown";
            return best_bomb;
        }
        
        for (const auto& primary_target : enemies) {
            if (!primary_target.is_alive()) continue;
            
            int throw_distance = abs(agent.x - primary_target.x) + abs(agent.y - primary_target.y);
            if (throw_distance > THROW_DISTANCE_MAX) continue;
            
            vector<int> targets_hit;
            int total_expected_damage = 0;
            double total_kill_probability = 0.0;
            
            for (const auto& enemy : enemies) {
                if (!enemy.is_alive()) continue;
                
                int splash_distance = abs(primary_target.x - enemy.x) + abs(primary_target.y - enemy.y);
                int damage = GameMechanics::calculate_exact_bomb_damage(splash_distance, false);
                
                if (damage > 0) {
                    targets_hit.push_back(enemy.agent_id);
                    total_expected_damage += damage;
                    total_kill_probability += GameMechanics::calculate_kill_probability(enemy.wetness, damage);
                }
            }
            
            if (!targets_hit.empty()) {
                double expected_value = total_expected_damage * 40.0;
                expected_value += total_kill_probability * 1500.0; 
                
                if (targets_hit.size() > 1) {
                    expected_value += targets_hit.size() * 1200.0;
                }
                
                if (expected_value > best_bomb.expected_value) {
                    best_bomb.action_type = "THROW";
                    best_bomb.target_x = primary_target.x;
                    best_bomb.target_y = primary_target.y;
                    best_bomb.expected_value = expected_value;
                    best_bomb.expected_damage = total_expected_damage;
                    best_bomb.kill_probability = total_kill_probability;
                    best_bomb.tactical_reasoning = "Bomb at (" + to_string(primary_target.x) + 
                        "," + to_string(primary_target.y) + ") hits " + to_string(targets_hit.size()) + 
                        " enemies for " + to_string(total_expected_damage) + " total damage";
                }
            }
        }
        
        return best_bomb;
    }

    TacticalDecision evaluate_cover_strategy(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision cover_decision;
        cover_decision.action_type = "HUNKER_DOWN";
        cover_decision.expected_value = 0;
        
        int immediate_threats = 0;
        int total_enemy_damage_potential = 0;
        bool under_heavy_fire = false;
        
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            
            if (distance <= 4) {
                immediate_threats++;
                total_enemy_damage_potential += 20;
            }
            if (enemy.splash_bombs > 0 && distance <= 4) {
                total_enemy_damage_potential += 30;
                under_heavy_fire = true;
            }
        }
        bool should_seek_cover = false;
        string cover_reason = "";
        if (agent.get_health() <= 50 && immediate_threats >= 2) {
            should_seek_cover = true;
            cover_reason = "Low health + multiple threats";
        }
        if (total_enemy_damage_potential >= 60) {
            should_seek_cover = true;
            cover_reason = "Heavy enemy fire incoming";
        }
        if (enemies.size() > allies.size() + 1) {
            should_seek_cover = true;
            cover_reason = "Outnumbered by enemies";
        }
        if (agent.get_health() <= 70 && under_heavy_fire) {
            should_seek_cover = true;
            cover_reason = "Wounded + bomb threats";
        }
        if (!should_seek_cover) {
            cover_decision.tactical_reasoning = "No need for cover - continue aggressive tactics";
            return cover_decision;
        }
        vector<pair<int, int>> cover_positions;
        for (int dx = -2; dx <= 2; dx++) {
            for (int dy = -2; dy <= 2; dy++) {
                int cx = agent.x + dx;
                int cy = agent.y + dy;
                
                if (GameMechanics::provides_cover(cx, cy, board_width, board_height, tile_map)) {
                    bool occupied = false;
                    for (const auto& ally : allies) {
                        if (ally.agent_id != agent.agent_id && ally.x == cx && ally.y == cy) {
                            occupied = true;
                            break;
                        }
                    }
                    for (const auto& enemy : enemies) {
                        if (enemy.x == cx && enemy.y == cy) {
                            occupied = true;
                            break;
                        }
                    }
                    
                    if (!occupied) {
                        cover_positions.push_back({cx, cy});
                    }
                }
            }
        }
        
        if (!cover_positions.empty()) {
            int best_distance = INT_MAX;
            pair<int, int> best_cover = {-1, -1};
            
            for (const auto& pos : cover_positions) {
                int distance = abs(agent.x - pos.first) + abs(agent.y - pos.second);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_cover = pos;
                }
            }
            
            if (best_cover.first != -1) {
                cover_decision.action_type = "MOVE";
                cover_decision.target_x = best_cover.first;
                cover_decision.target_y = best_cover.second;
                cover_decision.expected_value = 3000.0; 
                cover_decision.tactical_reasoning = "🛡️ SEEK COVER at (" + to_string(best_cover.first) + 
                    "," + to_string(best_cover.second) + ") - " + cover_reason;
                
                cerr << "🛡️ Agent " << agent.agent_id << " seeking cover: " << cover_reason << endl;
            }
        }
        
        return cover_decision;
    }
    
    TacticalDecision evaluate_sniper_strategy(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision sniper_decision;
        sniper_decision.action_type = "HUNKER_DOWN";
        sniper_decision.expected_value = 0;
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        if (data.agent_class != GameAgentClass::SNIPER) {
            sniper_decision.tactical_reasoning = "Not a sniper agent";
            return sniper_decision;
        }
        
        int my_team_health = 0;
        int enemy_team_health = 0;
        int bombers_nearby = 0;
        
        for (const auto& ally : allies) {
            my_team_health += ally.get_health();
        }
        
        for (const auto& enemy : enemies) {
            enemy_team_health += enemy.get_health();
            
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            if (enemy.splash_bombs > 0 && distance <= 6) {
                bombers_nearby++;
            }
        }
        
        
        bool team_advantage = (my_team_health >= enemy_team_health) && (allies.size() >= enemies.size());
        bool low_personal_health = agent.get_health() <= 60;
        bool bomber_threat = bombers_nearby > 0;
        
        
        bool should_keep_distance = false;
        string strategy_reason = "";
        
        if (!team_advantage && (low_personal_health || bomber_threat)) {
            should_keep_distance = true;
            strategy_reason = "Defensive: Team disadvantage + personal threats";
        } else if (bomber_threat && agent.get_health() <= 80) {
            should_keep_distance = true;
            strategy_reason = "Bomber threats detected - maintain safe distance";
        } else {
            should_keep_distance = false;
            strategy_reason = "Aggressive: Team advantage allows close engagement";
        }
        
        cerr << "🎯 SNIPER STRATEGY: " << strategy_reason << " (Team HP: " << my_team_health << " vs " << enemy_team_health << ")" << endl;
        
        if (should_keep_distance) {
            
            int optimal_distance = 5; 
            
            
            const AgentState* closest_enemy = nullptr;
            int min_distance = INT_MAX;
            
            for (const auto& enemy : enemies) {
                if (!enemy.is_alive()) continue;
                int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
                if (distance < min_distance) {
                    min_distance = distance;
                    closest_enemy = &enemy;
                }
            }
            
            if (closest_enemy != nullptr) {
                
                int dx = agent.x - closest_enemy->x;
                int dy = agent.y - closest_enemy->y;
                
                
                double length = sqrt(dx*dx + dy*dy);
                if (length > 0) {
                    int target_x = closest_enemy->x + (int)((dx/length) * optimal_distance);
                    int target_y = closest_enemy->y + (int)((dy/length) * optimal_distance);
                    
                    
                    target_x = max(0, min(board_width-1, target_x));
                    target_y = max(0, min(board_height-1, target_y));
                    
                    
                    vector<pair<int, int>> occupied;
                    for (const auto& ally : allies) {
                        if (ally.agent_id != agent.agent_id && ally.is_alive()) {
                            occupied.push_back({ally.x, ally.y});
                        }
                    }
                    
                    if (GameMechanics::is_valid_movement_position(target_x, target_y, board_width, board_height, tile_map, occupied)) {
                        sniper_decision.action_type = "MOVE";
                        sniper_decision.target_x = target_x;
                        sniper_decision.target_y = target_y;
                        sniper_decision.expected_value = 2500.0;
                        sniper_decision.tactical_reasoning = "🎯 SNIPER RETREAT to (" + to_string(target_x) + 
                            "," + to_string(target_y) + ") - " + strategy_reason;
                    }
                }
            }
        } else {
            
            sniper_decision.expected_value = 0; 
            sniper_decision.tactical_reasoning = "🎯 SNIPER AGGRESSIVE - using normal tactics";
        }
        
        return sniper_decision;
    }
    
    TacticalDecision evaluate_tactical_movement(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision best_move;
        best_move.action_type = "HUNKER_DOWN";
        best_move.expected_value = 0;
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        GameAgentClass agent_class = data.agent_class;
        
        
        vector<pair<int, int>> occupied;
        for (const auto& ally : allies) {
            if (ally.agent_id != agent.agent_id && ally.is_alive()) {
                occupied.push_back({ally.x, ally.y});
            }
        }
        for (const auto& enemy : enemies) {
            if (enemy.is_alive()) {
                occupied.push_back({enemy.x, enemy.y});
            }
        }
        
        
        int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};
        
        for (int i = 0; i < 8; i++) {
            int nx = agent.x + dx[i];
            int ny = agent.y + dy[i];
            
            
            if (nx < 0 || nx >= board_width || ny < 0 || ny >= board_height) continue;
            
            
            bool collision = false;
            for (const auto& pos : occupied) {
                if (pos.first == nx && pos.second == ny) {
                    collision = true;
                    break;
                }
            }
            if (collision) continue;
            
            double expected_value = 150.0; 
            
            
            int movement_cost = GameMechanics::calculate_movement_cost(agent.wetness);
            if (movement_cost > 1) {
                expected_value -= (movement_cost - 1) * 50.0; 
            }
            
            
            for (const auto& enemy : enemies) {
                if (!enemy.is_alive()) continue;
                
                int current_distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
                int new_distance = abs(nx - enemy.x) + abs(ny - enemy.y);
                
                
                if (new_distance <= data.optimal_range && current_distance > data.optimal_range) {
                    expected_value += 1000.0; 
                }
                
                
                if (new_distance <= data.optimal_range) {
                    expected_value += 500.0;
                }
                
                
                switch (agent_class) {
                    case GameAgentClass::SNIPER:
                        if (new_distance >= 4 && new_distance <= 6) expected_value += 700.0;
                        break;
                    case GameAgentClass::BOMBER:
                        if (new_distance <= 4) expected_value += 600.0;
                        break;
                    case GameAgentClass::BERSERKER:
                        if (new_distance <= 2) expected_value += 800.0;
                        break;
                    default:
                        if (new_distance <= 4) expected_value += 400.0;
                }
                
                
                if (enemy.wetness > 50 && new_distance < current_distance) {
                    expected_value += 300.0;
                }
            }
            
            
            if (expected_value > 1500.0) expected_value = 1500.0;
            
            if (expected_value > best_move.expected_value) {
                best_move.action_type = "MOVE";
                best_move.target_x = nx;
                best_move.target_y = ny;
                best_move.expected_value = expected_value;
                best_move.tactical_reasoning = "Move to (" + to_string(nx) + "," + to_string(ny) + 
                    ") for tactical advantage (value: " + to_string((int)expected_value) + ")";
            }
        }
        
        return best_move;
    }

    TacticalDecision make_optimal_decision(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        cerr << "Agent " << agent.agent_id << " (" << get_class_name(all_agents_data.at(agent.agent_id).agent_class) << ") ";
        cerr << "at (" << agent.x << "," << agent.y << ") HP=" << agent.get_health() << " CD=" << agent.cooldown << " Bombs=" << agent.splash_bombs << endl;
        
        bool critical_urgency = (agent.get_health() <= 40 && agent.splash_bombs > 0 && agent.cooldown == 0);
        if (critical_urgency) {
            cerr << "🚨 CRITICAL URGENCY: Low health + bombs available - PRIORITIZE BOMBING!" << endl;
        }
        
        TacticalDecision best_shoot = find_best_shooting_target(agent, enemies);
        TacticalDecision best_bomb = find_best_bombing_target_with_allies(agent, enemies, allies);
        
        if (critical_urgency && best_bomb.action_type == "THROW") {
            best_bomb.expected_value *= 5.0;
            best_bomb.tactical_reasoning = "🚨 CRITICAL BOMB: " + best_bomb.tactical_reasoning;
            cerr << "🚨 CRITICAL BOMB BOOST: " << (int)best_bomb.expected_value << endl;
        }
        
        TacticalDecision best_compound = find_best_compound_action(agent, enemies, allies);
        
        TacticalDecision cover_strategy = evaluate_cover_strategy(agent, enemies, allies);
        TacticalDecision sniper_strategy = evaluate_sniper_strategy(agent, enemies, allies);
        
        vector<TacticalDecision> movement_options = generate_random_moves(agent, enemies, allies, 50);
        
        vector<TacticalDecision> all_options = {best_shoot, best_bomb, best_compound, cover_strategy, sniper_strategy};
        all_options.insert(all_options.end(), movement_options.begin(), movement_options.end());
        
        TacticalDecision optimal = expectimax_evaluate(agent, all_options, enemies, allies);
        
        if (agent.splash_bombs > 0 && agent.get_health() <= 50 && best_bomb.action_type == "THROW") {
            if (best_bomb.expected_value > optimal.expected_value * 0.5) {
                cerr << "🧨 BOMB URGENCY OVERRIDE: Using bombs before death!" << endl;
                optimal = best_bomb;
            }
        }
        
        cerr << "FINAL DECISION: " << optimal.action_type << " (value: " << (int)optimal.expected_value << ")" << endl;
        
        return optimal;
    }
    
    TacticalDecision find_best_compound_action(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision best_compound;
        best_compound.action_type = "HUNKER_DOWN";
        best_compound.expected_value = 0;
        
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        
        vector<pair<int, int>> occupied;
        for (const auto& ally : allies) {
            if (ally.agent_id != agent.agent_id && ally.is_alive()) {
                occupied.push_back({ally.x, ally.y});
            }
        }
        for (const auto& enemy : enemies) {
            if (enemy.is_alive()) {
                occupied.push_back({enemy.x, enemy.y});
            }
        }
        
        vector<pair<int, int>> movement_priorities;
        
        const AgentState* closest_enemy = nullptr;
        int min_distance = INT_MAX;
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            if (distance < min_distance) {
                min_distance = distance;
                closest_enemy = &enemy;
            }
        }
        
        if (closest_enemy != nullptr) {
            
            int target_x = closest_enemy->x;
            int target_y = closest_enemy->y;
            
            
            for (int dx = -2; dx <= 2; dx++) {
                for (int dy = -2; dy <= 2; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    
                    int nx = agent.x + dx;
                    int ny = agent.y + dy;
                    
                    if (nx < 0 || nx >= board_width || ny < 0 || ny >= board_height) continue;
                    if (!GameMechanics::is_valid_movement_position(nx, ny, board_width, board_height, tile_map, occupied)) continue;
                    
                    int new_distance = abs(nx - target_x) + abs(ny - target_y);
                    
                    
                    if (new_distance < min_distance) {
                        
                        bool prefer_this_tile = true;
                        if (ny >= 0 && ny < tile_map.size() && nx >= 0 && nx < tile_map[0].size()) {
                            int tile_type = tile_map[ny][nx];
                            if (tile_type == 1) { 
                                prefer_this_tile = false;
                            }
                        }
                        
                        if (prefer_this_tile) {
                            movement_priorities.insert(movement_priorities.begin(), {nx, ny}); 
                        } else {
                            movement_priorities.push_back({nx, ny}); 
                        }
                    }
                }
            }
        }
        
        
        if (movement_priorities.empty()) {
            int dx[] = {1, 1, 0, -1, -1, -1, 0, 1}; 
            int dy[] = {0, 1, 1, 1, 0, -1, -1, -1};
            
            for (int i = 0; i < 8; i++) {
                int nx = agent.x + dx[i];
                int ny = agent.y + dy[i];
                
                if (nx >= 0 && nx < board_width && ny >= 0 && ny < board_height) {
                    if (GameMechanics::is_valid_movement_position(nx, ny, board_width, board_height, tile_map, occupied)) {
                        
                        if (ny >= 0 && ny < tile_map.size() && nx >= 0 && nx < tile_map[0].size()) {
                            int tile_type = tile_map[ny][nx];
                            if (tile_type == 0) { 
                                movement_priorities.insert(movement_priorities.begin(), {nx, ny});
                            } else {
                                movement_priorities.push_back({nx, ny}); 
                            }
                        } else {
                            movement_priorities.push_back({nx, ny});
                        }
                    }
                }
            }
        }
        
        
        for (const auto& pos : movement_priorities) {
            int nx = pos.first;
            int ny = pos.second;
            
            
            for (const auto& enemy : enemies) {
                if (!enemy.is_alive()) continue;
                
                int distance = abs(nx - enemy.x) + abs(ny - enemy.y);
                
                if (distance <= data.optimal_range) {
                    int base_damage = data.soaking_power;
                    
                    if (base_damage > 0) {
                        double expected_value = base_damage * 250.0;
                        if (enemy.wetness + base_damage >= 100) {
                            expected_value += 15000.0; 
                        } else {
                            expected_value += (enemy.wetness + base_damage) * 150.0;
                        }
                        int old_distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
                        if (distance < old_distance) {
                            expected_value += 2000.0; 
                        }
                        
                        if (expected_value > best_compound.expected_value) {
                            best_compound.action_type = "MOVE_SHOOT";
                            best_compound.target_x = nx;
                            best_compound.target_y = ny;
                            best_compound.target_agent_id = enemy.agent_id;
                            best_compound.expected_value = expected_value;
                            best_compound.expected_damage = base_damage;
                            best_compound.tactical_reasoning = "🚀 ADVANCE to (" + to_string(nx) + "," + to_string(ny) + 
                                ") + SHOOT enemy " + to_string(enemy.agent_id) + " for " + to_string(base_damage) + " damage";
                        }
                    }
                }
            }
            
            
            if (agent.splash_bombs > 0 && agent.wetness < 80) {
                
                for (const auto& target_enemy : enemies) {
                    if (!target_enemy.is_alive()) continue;
                    
                    
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            int bomb_x = target_enemy.x + dx;
                            int bomb_y = target_enemy.y + dy;
                            
                            
                            if (bomb_x < 0 || bomb_x >= board_width || bomb_y < 0 || bomb_y >= board_height) continue;
                            
                            
                            int throw_distance = abs(nx - bomb_x) + abs(ny - bomb_y);
                            if (throw_distance > THROW_DISTANCE_MAX) continue;
                            
                            
                            int total_damage = 0;
                            int enemies_hit = 0;
                            
                            for (const auto& enemy : enemies) {
                                if (!enemy.is_alive()) continue;
                                
                                int splash_distance = abs(bomb_x - enemy.x) + abs(bomb_y - enemy.y);
                                if (splash_distance <= 1) {
                                    total_damage += 30;
                                    enemies_hit++;
                                }
                            }
                            
                            if (total_damage > 0) {
                                
                                int self_damage = 0;
                                int agent_splash_distance = abs(bomb_x - nx) + abs(bomb_y - ny);
                                if (agent_splash_distance <= 1) {
                                    self_damage = 30;
                                    if (agent.wetness > 70) self_damage = 15;
                                }
                                
                                
                                if (total_damage > self_damage * 1.2) {
                                    double expected_value = total_damage * 200.0;
                                    expected_value -= self_damage * 100.0;
                                    
                                    if (enemies_hit > 1) {
                                        expected_value += enemies_hit * 4000.0;
                                    }
                                    
                                    
                                    int old_distance = abs(agent.x - target_enemy.x) + abs(agent.y - target_enemy.y);
                                    int new_distance = abs(nx - target_enemy.x) + abs(ny - target_enemy.y);
                                    if (new_distance < old_distance) {
                                        expected_value += 1500.0;
                                    }
                                    
                                    if (self_damage == 0) expected_value += 800.0;
                                    
                                    if (expected_value > best_compound.expected_value) {
                                        best_compound.action_type = "MOVE_THROW";
                                        best_compound.target_x = nx;
                                        best_compound.target_y = ny;
                                        best_compound.bomb_x = bomb_x;
                                        best_compound.bomb_y = bomb_y;
                                        best_compound.expected_value = expected_value;
                                        best_compound.expected_damage = total_damage;
                                        best_compound.tactical_reasoning = "🚀 ADVANCE to (" + to_string(nx) + "," + to_string(ny) + 
                                            ") + BOMB at (" + to_string(bomb_x) + "," + to_string(bomb_y) + 
                                            ") hits " + to_string(enemies_hit) + " enemies (throw_dist=" + to_string(throw_distance) + ")";
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        return best_compound;
    }
    
    TacticalDecision find_best_shooting_target(const AgentState& agent, const vector<AgentState>& enemies) {
        TacticalDecision best_shot;
        best_shot.action_type = "HUNKER_DOWN";
        best_shot.expected_value = 0;
        
        if (agent.cooldown > 0 || enemies.empty()) {
            best_shot.tactical_reasoning = "Cannot shoot - cooldown or no enemies";
            return best_shot;
        }
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        
        cerr << "Agent " << agent.agent_id << " shooting evaluation: range=" << data.optimal_range << " power=" << data.soaking_power << endl;
        
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            
            cerr << "  Enemy " << enemy.agent_id << " at distance " << distance << " vs optimal_range " << data.optimal_range << endl;
            
            
            if (distance > data.optimal_range * 2) {
                cerr << "    Out of range (distance=" << distance << " > max_range=" << (data.optimal_range * 2) << ")" << endl;
                continue; 
            }
            
            
            int base_damage = data.soaking_power;
            if (distance > data.optimal_range) {
                base_damage = base_damage / 2; 
            }
            
            
            double cover_multiplier = calculate_cover_protection(agent, enemy);
            int final_damage = (int)(base_damage * cover_multiplier);
            
            cerr << "    Base damage: " << base_damage << " cover_mult: " << cover_multiplier << " final: " << final_damage << endl;
            
            if (final_damage > 0) {
                
                double expected_value = final_damage * 150.0; 
                
                
                if (enemy.wetness + final_damage >= 100) {
                    expected_value += 8000.0; 
                } else {
                    expected_value += (enemy.wetness + final_damage) * 80.0; 
                }
                
                
                if (enemy.wetness > 50) expected_value *= 1.5;
                if (enemy.wetness > 80) expected_value *= 2.0;
                
                cerr << "    Expected value: " << (int)expected_value << endl;
                
                if (expected_value > best_shot.expected_value) {
                    best_shot.action_type = "SHOOT";
                    best_shot.target_agent_id = enemy.agent_id;
                    best_shot.expected_value = expected_value;
                    best_shot.expected_damage = final_damage;
                    best_shot.tactical_reasoning = "Focus fire on enemy " + to_string(enemy.agent_id) + 
                        " for " + to_string(final_damage) + " damage at distance " + to_string(distance);
                }
            }
        }
        
        if (best_shot.action_type == "HUNKER_DOWN") {
            best_shot.tactical_reasoning = "No enemies in effective shooting range";
        }
        
        return best_shot;
    }
    
    
    double calculate_cover_protection(const AgentState& shooter, const AgentState& target) {
        
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) continue;
                
                int cover_x = target.x + dx;
                int cover_y = target.y + dy;
                
                if (cover_x >= 0 && cover_x < board_width && cover_y >= 0 && cover_y < board_height) {
                    int cover_type = 0; 
                    if (cover_y < tile_map.size() && cover_x < tile_map[cover_y].size()) {
                        cover_type = tile_map[cover_y][cover_x];
                    }
                    
                    if (cover_type == 1 || cover_type == 2) { 
                        
                        bool blocks_shot = false;
                        
                        
                        if ((cover_x == target.x + 1 && shooter.x < target.x) ||
                            (cover_x == target.x - 1 && shooter.x > target.x) ||
                            (cover_y == target.y + 1 && shooter.y < target.y) ||
                            (cover_y == target.y - 1 && shooter.y > target.y)) {
                            blocks_shot = true;
                        }
                        
                        if (blocks_shot) {
                            return (cover_type == 1) ? 0.5 : 0.25; 
                        }
                    }
                }
            }
        }
        return 1.0; 
    }
    
    
    TacticalDecision find_best_bombing_target_with_allies(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision best_bomb;
        best_bomb.action_type = "HUNKER_DOWN";
        best_bomb.expected_value = 0;
        
        
        if (agent.cooldown > 0 || agent.splash_bombs <= 0) {
            best_bomb.tactical_reasoning = "Cannot bomb - cooldown or no bombs";
            return best_bomb;
        }
        
        cerr << "🧨 Agent " << agent.agent_id << " CLEAN BOMBING: bombs=" << agent.splash_bombs 
             << " health=" << agent.get_health() << endl;
        
        int best_x = -1, best_y = -1;
        int best_damage = 0;
        
        
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            
            int distance_to_enemy = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            
            if (distance_to_enemy <= THROW_DISTANCE_MAX) { 
                cerr << "  🎯 Enemy " << enemy.agent_id << " at (" << enemy.x << "," << enemy.y 
                     << ") distance=" << distance_to_enemy << " [REACHABLE]" << endl;
                
                
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        int bomb_x = enemy.x + dx;
                        int bomb_y = enemy.y + dy;
                        
                        
                        if (bomb_x < 0 || bomb_x >= board_width || bomb_y < 0 || bomb_y >= board_height) continue;
                        
                        
                        int throw_distance = abs(agent.x - bomb_x) + abs(agent.y - bomb_y);
                        if (throw_distance > THROW_DISTANCE_MAX) continue;
                        
                        
                        bool hits_friendly = false;
                        int friendly_hits = 0;
                        for (const auto& ally : allies) {
                            if (ally.agent_id != agent.agent_id && ally.is_alive()) {
                                if (abs(bomb_x - ally.x) + abs(bomb_y - ally.y) <= 1) {
                                    hits_friendly = true;
                                    friendly_hits++;
                                }
                            }
                        }
                        
                        if (hits_friendly) {
                            cerr << "    ❌ FRIENDLY FIRE: Bomb at (" << bomb_x << "," << bomb_y 
                                 << ") would hit " << friendly_hits << " allies" << endl;
                            continue; 
                        }
                        
                        int total_damage = calculate_total_splash_damage_clean(enemies, bomb_x, bomb_y);
                        
                        if (total_damage > best_damage) {
                            best_damage = total_damage;
                            best_x = bomb_x;
                            best_y = bomb_y;
                            
                            cerr << "    💥 NEW BEST: Bomb at (" << bomb_x << "," << bomb_y 
                                 << ") damage=" << total_damage << " throw_dist=" << throw_distance << endl;
                        }
                    }
                }
            } else {
                cerr << "  ❌ Enemy " << enemy.agent_id << " at distance=" << distance_to_enemy << " > max_throw=" << THROW_DISTANCE_MAX << endl;
            }
        }
        
        if (best_x != -1 && best_y != -1 && best_damage > 0) {
            double expected_value = best_damage * 20.0; 
            
            
            int enemies_hit = count_enemies_in_splash_clean(enemies, best_x, best_y);
            if (enemies_hit > 1) {
                expected_value += enemies_hit * 500.0;
                cerr << "  🎯 MULTI-TARGET: " << enemies_hit << " enemies hit!" << endl;
            }
            
            best_bomb.action_type = "THROW";
            best_bomb.target_x = best_x;
            best_bomb.target_y = best_y;
            best_bomb.expected_value = expected_value;
            best_bomb.expected_damage = best_damage;
            best_bomb.tactical_reasoning = "Clean bomb hits " + to_string(enemies_hit) + 
                " enemies for " + to_string(best_damage) + " total damage";
            
            cerr << "  ✅ BOMBING: (" << best_x << "," << best_y << ") expected_value=" << (int)expected_value << endl;
        } else {
            best_bomb.tactical_reasoning = "No valid bomb targets within range " + to_string(THROW_DISTANCE_MAX);
        }
        
        return best_bomb;
    }
    
    
    TacticalDecision find_best_bombing_target(const AgentState& agent, const vector<AgentState>& enemies) {
        vector<AgentState> empty_allies; 
        return find_best_bombing_target_with_allies(agent, enemies, empty_allies);
    }
    
    
    int calculate_total_splash_damage_clean(const vector<AgentState>& enemies, int bomb_x, int bomb_y) {
        int total_damage = 0;
        int friendly_hits = 0;
        
        
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            if (abs(bomb_x - enemy.x) + abs(bomb_y - enemy.y) <= 1) {
                total_damage += 30; 
            }
        }
        
        
        
        
        return max(0, total_damage);
    }
    
    
    int count_enemies_in_splash_clean(const vector<AgentState>& enemies, int bomb_x, int bomb_y) {
        int count = 0;
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            if (abs(bomb_x - enemy.x) + abs(bomb_y - enemy.y) <= 1) {
                count++;
            }
        }
        return count;
    }
    
    
    vector<TacticalDecision> generate_random_moves(const AgentState& agent, const vector<AgentState>& enemies, const vector<AgentState>& allies, int num_simulations) {
        vector<TacticalDecision> moves;
        
        
        vector<pair<int, int>> occupied;
        for (const auto& ally : allies) {
            if (ally.agent_id != agent.agent_id && ally.is_alive()) {
                occupied.push_back({ally.x, ally.y});
            }
        }
        for (const auto& enemy : enemies) {
            if (enemy.is_alive()) {
                occupied.push_back({enemy.x, enemy.y});
            }
        }
        
        
        const AgentState* priority_target = nullptr;
        int closest_distance = INT_MAX;
        for (const auto& enemy : enemies) {
            if (!enemy.is_alive()) continue;
            int distance = abs(agent.x - enemy.x) + abs(agent.y - enemy.y);
            if (distance < closest_distance) {
                closest_distance = distance;
                priority_target = &enemy;
            }
        }
        
        
        vector<pair<int, int>> movement_candidates;
        
        
        int dx[] = {1, 1, 0, -1, -1, -1, 0, 1};  
        int dy[] = {0, 1, 1, 1, 0, -1, -1, -1};
        
        for (int i = 0; i < 8; i++) {
            int nx = agent.x + dx[i];
            int ny = agent.y + dy[i];
            
            if (nx >= 0 && nx < board_width && ny >= 0 && ny < board_height) {
                movement_candidates.push_back({nx, ny});
            }
        }
        
        
        if (priority_target != nullptr) {
            for (int step = 1; step <= 2; step++) {
                for (int i = 0; i < 8; i++) {
                    int nx = agent.x + dx[i] * step;
                    int ny = agent.y + dy[i] * step;
                    
                    if (nx >= 0 && nx < board_width && ny >= 0 && ny < board_height) {
                        movement_candidates.push_back({nx, ny});
                    }
                }
            }
        }
        
        
        for (const auto& candidate : movement_candidates) {
            int nx = candidate.first;
            int ny = candidate.second;
            
            
            bool occupied_by_other = false;
            for (const auto& pos : occupied) {
                if (pos.first == nx && pos.second == ny) {
                    occupied_by_other = true;
                    break;
                }
            }
            
            if (occupied_by_other) {
                continue; 
            }
            
            TacticalDecision move_decision;
            move_decision.action_type = "MOVE";
            move_decision.target_x = nx;
            move_decision.target_y = ny;
            
            double expected_value = 300.0; 
            
            
            if (priority_target != nullptr) {
                int old_distance = abs(agent.x - priority_target->x) + abs(agent.y - priority_target->y);
                int new_distance = abs(nx - priority_target->x) + abs(ny - priority_target->y);
                
                
                if (new_distance < old_distance) {
                    expected_value += 2000.0; 
                }
                
                
                if (nx > agent.x) {
                    expected_value += 1500.0; 
                }
                
                
                const AgentData& data = all_agents_data.at(agent.agent_id);
                if (new_distance <= data.optimal_range) {
                    expected_value += 3000.0; 
                }
                if (new_distance <= data.optimal_range + 2) {
                    expected_value += 1000.0; 
                }
                
                
                if (new_distance > old_distance) {
                    expected_value -= 1000.0; 
                }
                
                
                if (nx < agent.x) {
                    expected_value -= 700.0; 
                }
            }
            
            move_decision.expected_value = expected_value;
            move_decision.tactical_reasoning = "Advance to (" + to_string(nx) + "," + to_string(ny) + 
                ") value=" + to_string((int)expected_value);
            
            moves.push_back(move_decision);
        }
        
        
        if (moves.empty()) {
            TacticalDecision hunker;
            hunker.action_type = "HUNKER_DOWN";
            hunker.expected_value = 200.0; 
            hunker.tactical_reasoning = "No valid moves - hunker down";
            moves.push_back(hunker);
        }
        
        return moves;
    }
    
    
    TacticalDecision expectimax_evaluate(const AgentState& agent, const vector<TacticalDecision>& options, const vector<AgentState>& enemies, const vector<AgentState>& allies) {
        TacticalDecision best_option;
        best_option.expected_value = -1000.0;
        
        for (const auto& option : options) {
            double total_value = option.expected_value;
            
            
            for (int sim = 0; sim < 10; sim++) { 
                double enemy_response_penalty = 0.0;
                
                
                for (const auto& enemy : enemies) {
                    if (!enemy.is_alive()) continue;
                    
                    int future_x = (option.action_type == "MOVE") ? option.target_x : agent.x;
                    int future_y = (option.action_type == "MOVE") ? option.target_y : agent.y;
                    
                    int distance_to_me = abs(future_x - enemy.x) + abs(future_y - enemy.y);
                    
                    
                    if (distance_to_me <= 4) { 
                        enemy_response_penalty += 100.0; 
                    }
                    if (distance_to_me <= 2) { 
                        enemy_response_penalty += 200.0; 
                    }
                }
                
                total_value -= enemy_response_penalty * 0.3; 
            }
            
            if (total_value > best_option.expected_value) {
                best_option = option;
                best_option.expected_value = total_value;
            }
        }
        
        
        if (best_option.expected_value < 0) {
            best_option.action_type = "HUNKER_DOWN";
            best_option.expected_value = 50.0;
            best_option.tactical_reasoning = "Expectimax fallback - hunker down";
        }
        
        return best_option;
    }
    
    
    
    
    
    struct SmitsimaxNode {
        
        vector<AgentState> my_agents;
        vector<AgentState> enemy_agents;
        
        
        vector<shared_ptr<SmitsimaxNode>> children;
        SmitsimaxNode* parent;
        
        
        vector<TacticalDecision> joint_action; 
        
        
        int visits;
        double total_reward;
        double ucb_value;
        
        
        double game_value;
        bool is_terminal;
        int depth;
        
        SmitsimaxNode(const vector<AgentState>& my_agents, const vector<AgentState>& enemy_agents, 
                     SmitsimaxNode* parent = nullptr, int depth = 0) 
            : my_agents(my_agents), enemy_agents(enemy_agents), parent(parent), 
              visits(0), total_reward(0.0), ucb_value(0.0), game_value(0.0), 
              is_terminal(false), depth(depth) {}
        
        
        double calculate_ucb(double exploration_constant = 1.414) const {
            if (visits == 0) return std::numeric_limits<double>::infinity();
            if (parent == nullptr || parent->visits == 0) return total_reward / visits;
            
            double exploitation = total_reward / visits;
            double exploration = exploration_constant * sqrt(log(parent->visits) / visits);
            return exploitation + exploration;
        }
        
        
        bool check_terminal() {
            
            bool my_agents_alive = false;
            bool enemy_agents_alive = false;
            
            for (const auto& agent : my_agents) {
                if (agent.is_alive()) my_agents_alive = true;
            }
            for (const auto& agent : enemy_agents) {
                if (agent.is_alive()) enemy_agents_alive = true;
            }
            
            is_terminal = !my_agents_alive || !enemy_agents_alive || depth >= 2; 
            return is_terminal;
        }
        
        
        double evaluate_state() {
            if (is_terminal) {
                
                int my_alive = 0, enemy_alive = 0;
                int my_health = 0, enemy_health = 0;
                
                for (const auto& agent : my_agents) {
                    if (agent.is_alive()) {
                        my_alive++;
                        my_health += agent.get_health();
                    }
                }
                for (const auto& agent : enemy_agents) {
                    if (agent.is_alive()) {
                        enemy_alive++;
                        enemy_health += agent.get_health();
                    }
                }
                
                
                if (my_alive > 0 && enemy_alive == 0) return 10000.0; 
                if (my_alive == 0 && enemy_alive > 0) return -10000.0; 
                
                
                double health_advantage = (my_health - enemy_health) * 10.0;
                double agent_advantage = (my_alive - enemy_alive) * 1000.0;
                
                return health_advantage + agent_advantage;
            }
            
            
            double value = 0.0;
            
            
            int my_alive = 0, enemy_alive = 0;
            int my_health = 0, enemy_health = 0;
            int my_bombs = 0, enemy_bombs = 0;
            
            for (const auto& agent : my_agents) {
                if (agent.is_alive()) {
                    my_alive++;
                    my_health += agent.get_health();
                    my_bombs += agent.splash_bombs;
                }
            }
            for (const auto& agent : enemy_agents) {
                if (agent.is_alive()) {
                    enemy_alive++;
                    enemy_health += agent.get_health();
                    enemy_bombs += agent.splash_bombs;
                }
            }
            
            
            double positional_value = 0.0;
            for (const auto& my_agent : my_agents) {
                if (!my_agent.is_alive()) continue;
                
                
                int min_distance = INT_MAX;
                for (const auto& enemy : enemy_agents) {
                    if (!enemy.is_alive()) continue;
                    int distance = abs(my_agent.x - enemy.x) + abs(my_agent.y - enemy.y);
                    min_distance = min(min_distance, distance);
                }
                
                
                if (min_distance <= 4) positional_value += 200.0;
                if (min_distance <= 2) positional_value += 100.0; 
            }
            
            value += (my_health - enemy_health) * 5.0;        
            value += (my_alive - enemy_alive) * 500.0;        
            value += (my_bombs - enemy_bombs) * 300.0;        
            value += positional_value;                        
            
            return value;
        }
    };
    
    
    class SmitsimaxSearch {
    private:
        SmartGameAI* ai_instance;
        random_device rd;
        mt19937 rng;
        
    public:
        SmitsimaxSearch(SmartGameAI* ai) : ai_instance(ai), rng(rd()) {}
        
        
        vector<AgentState> simulate_enemy_response_enhanced(const vector<AgentState>& enemies, 
                                                           const vector<AgentState>& my_agents,
                                                           bool use_game_folder = false) {
            if (use_game_folder) {
                
                cerr << "🎮 Game folder simulation not fully implemented - using internal simulation" << endl;
            }
            
            
            vector<AgentState> new_enemies = enemies;
            
            
            for (auto& enemy : new_enemies) {
                if (!enemy.is_alive()) continue;
                
                
                const AgentState* closest_target = nullptr;
                int min_distance = INT_MAX;
                
                for (const auto& my_agent : my_agents) {
                    if (!my_agent.is_alive()) continue;
                    int distance = abs(enemy.x - my_agent.x) + abs(enemy.y - my_agent.y);
                    if (distance < min_distance) {
                        min_distance = distance;
                        closest_target = &my_agent;
                    }
                }
                
                if (closest_target != nullptr) {
                    
                    if (enemy.x < closest_target->x) enemy.x++;
                    else if (enemy.x > closest_target->x) enemy.x--;
                    else if (enemy.y < closest_target->y) enemy.y++;
                    else if (enemy.y > closest_target->y) enemy.y--;
                }
            }
            
            return new_enemies;
        }
        
        
        vector<vector<TacticalDecision>> generate_joint_actions(const vector<AgentState>& my_agents, 
                                                               const vector<AgentState>& enemies,
                                                               const vector<AgentState>& all_allies) {
            vector<vector<TacticalDecision>> joint_actions;
            
            
            vector<vector<TacticalDecision>> agent_actions(my_agents.size());
            
            for (size_t i = 0; i < my_agents.size(); i++) {
                const auto& agent = my_agents[i];
                if (!agent.is_alive()) {
                    
                    TacticalDecision dead_action;
                    dead_action.action_type = "HUNKER_DOWN";
                    dead_action.expected_value = 0;
                    agent_actions[i].push_back(dead_action);
                    continue;
                }
                
                
                vector<TacticalDecision> actions;
                
                
                TacticalDecision shoot = ai_instance->find_best_shooting_target(agent, enemies);
                if (shoot.action_type == "SHOOT") {
                    actions.push_back(shoot);
                }
                
                
                TacticalDecision bomb = ai_instance->find_best_bombing_target_with_allies(agent, enemies, all_allies);
                if (bomb.action_type == "THROW") {
                    actions.push_back(bomb);
                }
                
                
                vector<TacticalDecision> moves = ai_instance->generate_random_moves(agent, enemies, all_allies, 2);
                for (size_t j = 0; j < min(size_t(2), moves.size()); j++) { 
                    actions.push_back(moves[j]);
                }
                
                
                TacticalDecision hunker;
                hunker.action_type = "HUNKER_DOWN";
                hunker.expected_value = 50.0;
                actions.push_back(hunker);
                
                agent_actions[i] = actions;
            }
            
            
            vector<int> indices(my_agents.size(), 0);
            
            do {
                vector<TacticalDecision> joint_action;
                for (size_t i = 0; i < my_agents.size(); i++) {
                    joint_action.push_back(agent_actions[i][indices[i]]);
                }
                joint_actions.push_back(joint_action);
                
                
                int carry = 1;
                for (int i = my_agents.size() - 1; i >= 0 && carry; i--) {
                    indices[i] += carry;
                    if (indices[i] >= (int)agent_actions[i].size()) {
                        indices[i] = 0;
                    } else {
                        carry = 0;
                    }
                }
                
                
                if (joint_actions.size() >= 16) break; 
                
            } while (indices[0] < (int)agent_actions[0].size() || 
                    any_of(indices.begin() + 1, indices.end(), [&](int idx) { return idx > 0; }));
            
            return joint_actions;
        }
        
        
        vector<AgentState> simulate_enemy_response(const vector<AgentState>& enemies, 
                                                  const vector<AgentState>& my_agents) {
            vector<AgentState> new_enemies = enemies;
            
            
            for (auto& enemy : new_enemies) {
                if (!enemy.is_alive()) continue;
                
                
                const AgentState* closest_target = nullptr;
                int min_distance = INT_MAX;
                
                for (const auto& my_agent : my_agents) {
                    if (!my_agent.is_alive()) continue;
                    int distance = abs(enemy.x - my_agent.x) + abs(enemy.y - my_agent.y);
                    if (distance < min_distance) {
                        min_distance = distance;
                        closest_target = &my_agent;
                    }
                }
                
                if (closest_target != nullptr) {
                    
                    if (enemy.x < closest_target->x && enemy.x < ai_instance->board_width - 1) enemy.x++;
                    else if (enemy.x > closest_target->x && enemy.x > 0) enemy.x--;
                    else if (enemy.y < closest_target->y && enemy.y < ai_instance->board_height - 1) enemy.y++;
                    else if (enemy.y > closest_target->y && enemy.y > 0) enemy.y--;
                }
            }
            
            return new_enemies;
        }
        
        
        vector<AgentState> apply_joint_action(const vector<AgentState>& agents, 
                                            const vector<TacticalDecision>& actions,
                                            const vector<AgentState>& enemies) {
            vector<AgentState> new_agents = agents;
            
            for (size_t i = 0; i < new_agents.size() && i < actions.size(); i++) {
                auto& agent = new_agents[i];
                const auto& action = actions[i];
                
                if (!agent.is_alive()) continue;
                
                
                if (action.action_type == "MOVE") {
                    agent.x = action.target_x;
                    agent.y = action.target_y;
                } else if (action.action_type == "SHOOT" && agent.cooldown == 0) {
                    agent.cooldown = 1; 
                } else if (action.action_type == "THROW" && agent.splash_bombs > 0) {
                    agent.splash_bombs--; 
                    agent.cooldown = 2; 
                }
                
                
                if (agent.cooldown > 0) agent.cooldown--;
            }
            
            return new_agents;
        }
        
        
        vector<TacticalDecision> smitsimax_search(const vector<AgentState>& my_agents,
                                                 const vector<AgentState>& enemies, 
                                                 int max_iterations = 30,
                                                 double time_limit_ms = 40.0) {
            
            auto start_time = chrono::high_resolution_clock::now();
            
            
            auto root = make_shared<SmitsimaxNode>(my_agents, enemies);
            
            cerr << "🔍 SMITSIMAX FAST: Starting search with " << my_agents.size() << " agents, " 
                 << max_iterations << " iterations, " << time_limit_ms << "ms limit" << endl;
            
            for (int iteration = 0; iteration < max_iterations; iteration++) {
                
                if (iteration % 5 == 0) {
                    auto current_time = chrono::high_resolution_clock::now();
                    auto elapsed = chrono::duration_cast<chrono::milliseconds>(current_time - start_time);
                    if (elapsed.count() > time_limit_ms) {
                        cerr << "🕐 SMITSIMAX: Time limit reached at iteration " << iteration << endl;
                        break;
                    }
                }
                
                
                shared_ptr<SmitsimaxNode> current = root;
                while (!current->children.empty() && !current->check_terminal()) {
                    
                    auto best_child = max_element(current->children.begin(), current->children.end(),
                        [](const shared_ptr<SmitsimaxNode>& a, const shared_ptr<SmitsimaxNode>& b) {
                            return a->calculate_ucb() < b->calculate_ucb();
                        });
                    current = *best_child;
                }
                
                
                if (!current->check_terminal() && current->visits > 0) {
                    auto joint_actions = generate_joint_actions(current->my_agents, current->enemy_agents, current->my_agents);
                    
                    for (const auto& joint_action : joint_actions) {
                        
                        auto new_my_agents = apply_joint_action(current->my_agents, joint_action, current->enemy_agents);
                        auto new_enemies = this->simulate_enemy_response_enhanced(current->enemy_agents, new_my_agents, false);
                        
                        auto child = make_shared<SmitsimaxNode>(new_my_agents, new_enemies, current.get(), current->depth + 1);
                        child->joint_action = joint_action;
                        current->children.push_back(child);
                        
                        
                        if (current->children.size() >= 8) break; 
                    }
                    
                    if (!current->children.empty()) {
                        current = current->children[0]; 
                    }
                }
                
                
                double value = current->evaluate_state();
                
                
                shared_ptr<SmitsimaxNode> backprop = current;
                while (backprop != nullptr) {
                    backprop->visits++;
                    backprop->total_reward += value;
                    
                    
                    if (backprop->parent == nullptr) break;
                    
                    
                    shared_ptr<SmitsimaxNode> parent_shared = nullptr;
                    if (backprop->parent == root.get()) {
                        parent_shared = root;
                    } else {
                        
                        break; 
                    }
                    backprop = parent_shared;
                }
            }
            
            
            if (root->children.empty()) {
                
                vector<TacticalDecision> fallback(my_agents.size());
                for (size_t i = 0; i < my_agents.size(); i++) {
                    fallback[i].action_type = "HUNKER_DOWN";
                    fallback[i].expected_value = 50.0;
                }
                cerr << "🚨 SMITSIMAX: No children generated - using fallback" << endl;
                return fallback;
            }
            
            auto best_child = max_element(root->children.begin(), root->children.end(),
                [](const shared_ptr<SmitsimaxNode>& a, const shared_ptr<SmitsimaxNode>& b) {
                    return a->visits < b->visits;
                });
            
            cerr << "✅ SMITSIMAX: Selected action with " << (*best_child)->visits 
                 << " visits, value " << (int)((*best_child)->total_reward / std::max(1, (*best_child)->visits)) << endl;
            
            return (*best_child)->joint_action;
        }
    };
    
    
    TacticalDecision evaluate_focus_fire(const AgentState& agent, const AgentState& priority_target) {
        TacticalDecision focus_decision;
        focus_decision.action_type = "HUNKER_DOWN";
        focus_decision.expected_value = 0;
        
        const AgentData& data = all_agents_data.at(agent.agent_id);
        int distance = abs(agent.x - priority_target.x) + abs(agent.y - priority_target.y);
        
        
        if (agent.cooldown == 0 && distance <= data.optimal_range) { 
            int base_damage = data.soaking_power; 
            
            cerr << "Focus fire evaluation: Agent " << agent.agent_id << " vs enemy " << priority_target.agent_id << " distance=" << distance << " optimal_range=" << data.optimal_range << " damage=" << base_damage << endl;
            
            if (base_damage > 0) {
                double expected_value = base_damage * 200.0; 
                
                
                if (priority_target.wetness + base_damage >= 100) {
                    expected_value += 10000.0; 
                } else {
                    expected_value += (priority_target.wetness + base_damage) * 100.0;
                }
                
                focus_decision.action_type = "SHOOT";
                focus_decision.target_agent_id = priority_target.agent_id;
                focus_decision.expected_value = expected_value;
                focus_decision.expected_damage = base_damage;
                focus_decision.tactical_reasoning = "🔥 FOCUS FIRE on priority target " + 
                    to_string(priority_target.agent_id) + " for " + to_string(base_damage) + " damage";
            }
        }
        
        
        if (agent.splash_bombs > 0 && agent.wetness < 80) {
            int bomb_throw_distance = abs(agent.x - priority_target.x) + abs(agent.y - priority_target.y);
            
            if (bomb_throw_distance <= THROW_DISTANCE_MAX) { 
                int bomb_damage = 30; 
                double expected_value = bomb_damage * 150.0; 
                
                if (priority_target.wetness + bomb_damage >= 100) {
                    expected_value += 8000.0; 
                }
                
                cerr << "Focus bomb evaluation: Agent " << agent.agent_id << " vs target at (" << priority_target.x << "," << priority_target.y << ") distance=" << bomb_throw_distance << " max=" << THROW_DISTANCE_MAX << endl;
                
                if (expected_value > focus_decision.expected_value) {
                    focus_decision.action_type = "THROW";
                    focus_decision.target_x = priority_target.x;
                    focus_decision.target_y = priority_target.y;
                    focus_decision.expected_value = expected_value;
                    focus_decision.expected_damage = bomb_damage;
                    focus_decision.tactical_reasoning = "🔥 FOCUS BOMB on priority target at (" + 
                        to_string(priority_target.x) + "," + to_string(priority_target.y) + ")";
                }
            } else {
                cerr << "Focus bomb out of range: distance=" << bomb_throw_distance << " > max=" << THROW_DISTANCE_MAX << endl;
            }
        }
        
        return focus_decision;
    }

    string format_compound_action(int agent_id, const TacticalDecision& decision) {
        if (decision.action_type == "SHOOT") {
            return to_string(agent_id) + ";SHOOT " + to_string(decision.target_agent_id) + "; HUNKER_DOWN";
        } else if (decision.action_type == "MOVE") {
            return to_string(agent_id) + ";MOVE " + to_string(decision.target_x) + " " + to_string(decision.target_y) + "; HUNKER_DOWN";
        } else if (decision.action_type == "THROW") {
            return to_string(agent_id) + ";THROW " + to_string(decision.target_x) + " " + to_string(decision.target_y) + "; HUNKER_DOWN";
        } else if (decision.action_type == "MOVE_SHOOT") {
            return to_string(agent_id) + ";MOVE " + to_string(decision.target_x) + " " + to_string(decision.target_y) + 
                   "; SHOOT " + to_string(decision.target_agent_id);
        } else if (decision.action_type == "MOVE_THROW") {
            return to_string(agent_id) + ";MOVE " + to_string(decision.target_x) + " " + to_string(decision.target_y) + 
                   "; THROW " + to_string(decision.bomb_x) + " " + to_string(decision.bomb_y);
        } else {
            return to_string(agent_id) + ";HUNKER_DOWN";
        }
    }
};

int main() {
    SmartGameAI ai;
    auto game_start = chrono::high_resolution_clock::now();
    
    cerr << "=== SMART GAME AI WITH EXACT MECHANICS ===" << endl;
    cerr << "Based on converted Java game source code" << endl;
    cerr << "Knows exact damage, collision, and tactical calculations" << endl;
    
    
    int my_id;
    cin >> my_id;
    cin.ignore();
    
    int agent_data_count;
    cin >> agent_data_count;
    cin.ignore();
    
    
    for (int i = 0; i < agent_data_count; i++) {
        SmartGameAI::AgentData agent;
        cin >> agent.agent_id >> agent.player >> agent.shoot_cooldown 
            >> agent.optimal_range >> agent.soaking_power >> agent.splash_bombs;
        cin.ignore();
        
        agent.agent_class = ai.determine_agent_class(agent);
        ai.all_agents_data[agent.agent_id] = agent;
        
        if (agent.player == my_id) {
            ai.my_agent_ids.push_back(agent.agent_id);
        } else {
            ai.enemy_agent_ids.push_back(agent.agent_id);
        }
    }
    
    
    cin >> ai.board_width >> ai.board_height;
    cin.ignore();
    
    
    vector<vector<int>> tile_map(ai.board_height, vector<int>(ai.board_width, 0));
    for (int i = 0; i < ai.board_height; i++) {
        for (int j = 0; j < ai.board_width; j++) {
            int x, y, tile_type;
            cin >> x >> y >> tile_type;
            cin.ignore();
            
            if (x >= 0 && x < ai.board_width && y >= 0 && y < ai.board_height) {
                tile_map[y][x] = tile_type;
            }
        }
    }
    
    
    ai.tile_map = tile_map;
    
    cerr << "=== INITIALIZATION COMPLETE ===" << endl;
    cerr << "My ID: " << my_id << endl;
    cerr << "Board: " << ai.board_width << "x" << ai.board_height << endl;
    cerr << "My agents: ";
    for (int id : ai.my_agent_ids) {
        cerr << id << "(" << ai.get_class_name(ai.all_agents_data[id].agent_class) << ") ";
    }
    cerr << endl;
    
    
    int turn_number = 0;
    while (true) {
        turn_number++;
        auto turn_start = chrono::high_resolution_clock::now();
        
        cerr << "=== TURN " << turn_number << " START ===" << endl;
        
        try {
            
            int agent_count;
            cin >> agent_count;
            if (cin.fail() || cin.eof()) break;
            cin.ignore();
            
            vector<SmartGameAI::AgentState> current_my_agents;
            vector<SmartGameAI::AgentState> current_enemy_agents;
            
            for (int i = 0; i < agent_count; i++) {
                SmartGameAI::AgentState agent;
                cin >> agent.agent_id >> agent.x >> agent.y
                    >> agent.cooldown >> agent.splash_bombs >> agent.wetness;
                cin.ignore();
                
                bool is_my_agent = find(ai.my_agent_ids.begin(), ai.my_agent_ids.end(), agent.agent_id) != ai.my_agent_ids.end();
                
                if (is_my_agent) {
                    current_my_agents.push_back(agent);
                    cerr << "MY AGENT: " << agent.agent_id << " at (" << agent.x << "," << agent.y << ") HP=" << (100-agent.wetness) << endl;
                } else {
                    current_enemy_agents.push_back(agent);
                    cerr << "ENEMY AGENT: " << agent.agent_id << " at (" << agent.x << "," << agent.y << ") HP=" << (100-agent.wetness) << endl;
                }
            }
            
            cerr << "Total enemies found: " << current_enemy_agents.size() << endl;
            
            int my_agent_count;
            cin >> my_agent_count;
            cin.ignore();
            
            cerr << "Expected " << my_agent_count << " output lines" << endl;
            
            
            int my_total_health = 0, enemy_total_health = 0;
            for (const auto& agent : current_my_agents) my_total_health += agent.get_health();
            for (const auto& agent : current_enemy_agents) enemy_total_health += agent.get_health();
            
            double tactical_advantage = GameMechanics::calculate_tactical_advantage(
                current_my_agents.size(), current_enemy_agents.size(), my_total_health, enemy_total_health);
            
            cerr << "Tactical advantage: " << (int)(tactical_advantage * 100) << "%" << endl;
            
            
            map<int, SmartGameAI::TacticalDecision> agent_decisions;
            
            
            set<pair<int, int>> movement_blacklist;
            
            
            const SmartGameAI::AgentState* priority_target = nullptr;
            double best_target_score = -1000.0;
            
            for (const auto& enemy : current_enemy_agents) {
                
                int min_distance_to_enemy = INT_MAX;
                for (const auto& my_agent : current_my_agents) {
                    int distance = abs(my_agent.x - enemy.x) + abs(my_agent.y - enemy.y);
                    min_distance_to_enemy = min(min_distance_to_enemy, distance);
                }
                
                
                double target_score = 0.0;
                
                
                if (enemy.splash_bombs > 0) {
                    target_score += enemy.splash_bombs * 3000.0; 
                    cerr << "Enemy " << enemy.agent_id << " has " << enemy.splash_bombs << " bombs (+3000 each)" << endl;
                }
                
                
                if (enemy.wetness > 50) {
                    target_score += (enemy.wetness - 50) * 60.0; 
                    cerr << "Enemy " << enemy.agent_id << " wounded (" << enemy.wetness << " wetness)" << endl;
                }
                
                
                target_score += (10 - min_distance_to_enemy) * 100.0; 
                
                
                if (enemy.cooldown <= 1) {
                    target_score += 1500.0; 
                }
                
                cerr << "Enemy " << enemy.agent_id << " score: " << (int)target_score 
                     << " (distance:" << min_distance_to_enemy << " bombs:" << enemy.splash_bombs 
                     << " wetness:" << enemy.wetness << " cd:" << enemy.cooldown << ")" << endl;
                
                if (target_score > best_target_score) {
                    best_target_score = target_score;
                    priority_target = &enemy;
                }
            }
            
            if (priority_target != nullptr) {
                
                int min_distance = INT_MAX;
                for (const auto& my_agent : current_my_agents) {
                    int distance = abs(my_agent.x - priority_target->x) + abs(my_agent.y - priority_target->y);
                    min_distance = min(min_distance, distance);
                }
                cerr << "🎯 FOCUS FIRE TARGET: Enemy " << priority_target->agent_id << " at (" 
                     << priority_target->x << "," << priority_target->y << ") distance=" << min_distance 
                     << " score=" << (int)best_target_score << endl;
            }
            
            
            bool use_smitsimax = (current_my_agents.size() >= 2 && current_enemy_agents.size() >= 1 && turn_number >= 3);
            
            if (use_smitsimax) {
                cerr << "🔍 USING SMITSIMAX: Multi-agent coordination for " << current_my_agents.size() << " agents" << endl;
                
                
                SmartGameAI::GameSimulator game_sim;
                
                
                vector<vector<int>> dummy_tile_map; 
                game_sim.save_game_state(current_my_agents, current_enemy_agents, 
                                       16, 16, dummy_tile_map); 
                
                SmartGameAI::SmitsimaxSearch search(&ai);
                vector<SmartGameAI::TacticalDecision> joint_actions = search.smitsimax_search(
                    current_my_agents, current_enemy_agents, 20, 30.0); 
                
                
                for (size_t i = 0; i < current_my_agents.size() && i < joint_actions.size(); i++) {
                    agent_decisions[current_my_agents[i].agent_id] = joint_actions[i];
                    cerr << "🎯 SMITSIMAX Agent " << current_my_agents[i].agent_id << ": " 
                         << joint_actions[i].action_type << " (value: " << (int)joint_actions[i].expected_value << ")" << endl;
                }
            } else {
                cerr << "🎮 USING INDIVIDUAL: Standard agent decisions" << endl;
            }
            
            for (const auto& agent : current_my_agents) {
                if (agent.is_alive()) {
                    SmartGameAI::TacticalDecision decision;
                    
                    
                    if (use_smitsimax && agent_decisions.count(agent.agent_id)) {
                        decision = agent_decisions[agent.agent_id];
                        cerr << "🔍 Agent " << agent.agent_id << " using SMITSIMAX decision: " << decision.action_type << endl;
                    } else {
                        decision = ai.make_optimal_decision(agent, current_enemy_agents, current_my_agents);
                        cerr << "🎮 Agent " << agent.agent_id << " using INDIVIDUAL decision: " << decision.action_type << endl;
                    }
                    
                    
                    if (decision.action_type == "MOVE" || decision.action_type == "MOVE_SHOOT" || decision.action_type == "MOVE_THROW") {
                        pair<int, int> target_pos = {decision.target_x, decision.target_y};
                        
                        if (movement_blacklist.count(target_pos)) {
                            cerr << "🚫 Agent " << agent.agent_id << " collision detected at (" << decision.target_x << "," << decision.target_y << ") - finding alternative" << endl;
                            
                            
                            bool found_alternative = false;
                            int dx[] = {1, -1, 0, 0, 1, -1, 1, -1};
                            int dy[] = {0, 0, 1, -1, 1, 1, -1, -1};
                            
                            for (int i = 0; i < 8 && !found_alternative; i++) {
                                int alt_x = decision.target_x + dx[i];
                                int alt_y = decision.target_y + dy[i];
                                pair<int, int> alt_pos = {alt_x, alt_y};
                                
                                if (alt_x >= 0 && alt_x < ai.board_width && alt_y >= 0 && alt_y < ai.board_height) {
                                    if (!movement_blacklist.count(alt_pos)) 
                                    {
                                        bool occupied = false;
                                        for (const auto& other : current_my_agents) {
                                            if (other.agent_id != agent.agent_id && other.x == alt_x && other.y == alt_y) {
                                                occupied = true;
                                                break;
                                            }
                                        }
                                        for (const auto& enemy : current_enemy_agents) {
                                            if (enemy.x == alt_x && enemy.y == alt_y) {
                                                occupied = true;
                                                break;
                                            }
                                        }
                                        
                                        if (!occupied) {
                                            cerr << "✅ Alternative found: (" << alt_x << "," << alt_y << ")" << endl;
                                            decision.target_x = alt_x;
                                            decision.target_y = alt_y;
                                            movement_blacklist.insert(alt_pos);
                                            found_alternative = true;
                                        }
                                    }
                                }
                            }
                            
                            if (!found_alternative) {
                                cerr << "⚠️ No alternative found - agent will hunker down" << endl;
                                decision.action_type = "HUNKER_DOWN";
                                decision.tactical_reasoning = "Collision avoidance - no safe move";
                            }
                        } else {
                            movement_blacklist.insert(target_pos);
                        }
                    }
                    if (priority_target != nullptr && agent.cooldown == 0)
                    {
                        SmartGameAI::TacticalDecision focus_fire = ai.evaluate_focus_fire(agent, *priority_target);
                        if (focus_fire.expected_value > decision.expected_value * 0.8) { 
                            decision = focus_fire;
                            cerr << "🔥 Agent " << agent.agent_id << " FOCUS FIRING on priority target!" << endl;
                        }
                    }
                    agent_decisions[agent.agent_id] = decision;
                } else {
                    SmartGameAI::TacticalDecision dead_decision;
                    dead_decision.action_type = "HUNKER_DOWN";
                    dead_decision.tactical_reasoning = "Agent is dead";
                    agent_decisions[agent.agent_id] = dead_decision;
                }
            }
            vector<SmartGameAI::AgentState> alive_agents;
            for (const auto& agent : current_my_agents)
            {
                if (agent.is_alive())
                    alive_agents.push_back(agent);
            }
            
            cerr << "=== OUTPUTTING " << my_agent_count << " TACTICAL COMMANDS ===" << endl;
            cerr << "Alive agents: " << alive_agents.size() << ", Expected output lines: " << my_agent_count << endl;
            
            for (int line = 0; line < my_agent_count; line++) {
                if (line < alive_agents.size()) {
                    int agent_id = alive_agents[line].agent_id;
                    SmartGameAI::TacticalDecision decision;
                    
                    if (agent_decisions.count(agent_id)) {
                        decision = agent_decisions[agent_id];
                    } else {
                        decision.action_type = "HUNKER_DOWN";
                        decision.tactical_reasoning = "Default defensive action";
                    }
                    
                    string action_line = ai.format_compound_action(agent_id, decision);
                    cout << action_line << endl;
                    cerr << "Line " << (line+1) << "/" << my_agent_count << ": " << action_line << " (Agent " << agent_id << ")" << endl;
                } 
                else {
                    int fallback_agent_id = alive_agents.empty() ? ai.my_agent_ids[0] : alive_agents[0].agent_id;
                    cout << fallback_agent_id << ";HUNKER_DOWN" << endl;
                    cerr << "Line " << (line+1) << "/" << my_agent_count << ": " << fallback_agent_id << ";HUNKER_DOWN (fallback)" << endl;
                }
            }
            
        } catch (const exception& e) {
            cerr << "EXCEPTION: " << e.what() << endl;
            for (int i = 0; i < (int)ai.my_agent_ids.size(); i++) {
                cout << to_string(ai.my_agent_ids[i]) << ";HUNKER_DOWN" << endl;
            }
        }
        
        cout.flush();
        cerr.flush();
        
        auto turn_end = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(turn_end - turn_start);
        cerr << "Turn " << turn_number << " completed in " << duration.count() << "ms" << endl;
        cerr << "========================================" << endl << endl;
    }
    return 0;
}
