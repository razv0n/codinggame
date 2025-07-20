#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <map>
#include <climits>
#include <queue>
#include <random>
#include <unordered_map>
using namespace std;

// MERGED SMITSIMAX + TACTICAL AI
// Combines multi-tree UCB search with comprehensive tactical evaluation
// Priority scoring system (-1.0 to 1.0) with agent class strategies

const int MAX_SEARCH_DEPTH = 6; // Balanced for performance
const double EXPLORATION_PARAM = 1.4; // UCB exploration parameter
const int MIN_RANDOM_VISITS = 8; // Random selection for first N visits
const int MAX_SIMULATION_TIME = 85; // milliseconds - leave buffer for tactical evaluation

// Agent class types from game
enum AgentClass {
    GUNNER = 0,   // cooldown=1, power=16, range=4, balloons=1
    SNIPER = 1,   // cooldown=5, power=24, range=6, balloons=0  
    BOMBER = 2,   // cooldown=2, power=8,  range=2, balloons=3
    ASSAULT = 3,  // cooldown=2, power=16, range=4, balloons=2
    BERSERKER = 4 // cooldown=5, power=32, range=2, balloons=1
};

struct AgentData {
    int agent_id;
    int player;
    int shoot_cooldown;
    int optimal_range;
    int soaking_power;
    int splash_bombs;
    AgentClass agent_class;
};

struct AgentState {
    int agent_id;
    int x, y;
    int cooldown;
    int splash_bombs;
    int wetness;
    
    // Constructor for easy copying
    AgentState() = default;
    AgentState(const AgentState& other) = default;
    AgentState& operator=(const AgentState& other) = default;
};

struct TacticalAction {
    string type;
    int target_id = -1;
    int target_x = -1;
    int target_y = -1;
    double priority_score = 0.0; // -1.0 to 1.0 range
    string reasoning = "";
};

// Smitsimax Node - represents a move choice in the agent's tree
struct SmitsimaxNode {
    SmitsimaxNode* parent;
    vector<SmitsimaxNode*> children;
    
    double total_score;
    int visits;
    
    // Move data (what this node represents)
    string action_type; // "SHOOT", "MOVE", "THROW", "HUNKER_DOWN"
    int target_x, target_y;
    int target_agent_id;
    
    // Tactical evaluation data
    double tactical_priority;
    
    SmitsimaxNode() : parent(nullptr), total_score(0.0), visits(0), 
                     action_type("HUNKER_DOWN"), target_x(-1), target_y(-1), 
                     target_agent_id(-1), tactical_priority(0.0) {}
    
    ~SmitsimaxNode() {
        for (auto* child : children) {
            delete child;
        }
    }
    
    double get_average_score() const {
        return visits > 0 ? total_score / visits : 0.0;
    }
};

int manhattan_distance(int x1, int y1, int x2, int y2) {
    return abs(x1 - x2) + abs(y1 - y2);
}

// Determine agent class from stats
AgentClass determine_agent_class(const AgentData& data) {
    if (data.optimal_range == 6 && data.soaking_power == 24) return SNIPER;
    if (data.optimal_range == 2 && data.splash_bombs >= 3) return BOMBER;
    if (data.optimal_range == 2 && data.soaking_power == 32) return BERSERKER;
    if (data.optimal_range == 4 && data.splash_bombs >= 2) return ASSAULT;
    return GUNNER; // Default
}

// Calculate shooting damage with range penalties
int calculate_shooting_damage(const AgentData& shooter, const AgentState& target, int distance) {
    if (distance > shooter.optimal_range) return 0;
    
    int base_damage = shooter.soaking_power;
    
    // Distance penalty for non-optimal range
    if (distance > 1) {
        double penalty = 0.25 * (distance - 1);
        base_damage = (int)(base_damage * (1.0 - penalty));
    }
    
    return max(0, base_damage);
}

// Calculate bomb/throw damage and splash
int calculate_throw_damage(const AgentData& thrower, int distance, bool is_splash = false) {
    if (thrower.splash_bombs <= 0) return 0;
    
    int base_damage = thrower.soaking_power;
    if (is_splash) base_damage /= 2; // Splash damage is halved
    
    // Throwing has different range mechanics than shooting
    if (distance > thrower.optimal_range * 2) return 0;
    
    return max(0, base_damage);
}

// Evaluate tile strategic value (from tactical AI)
double evaluate_tile_strategic_value(int x, int y, int width, int height, 
                                   const vector<AgentState>& my_agents,
                                   const vector<AgentState>& enemy_agents,
                                   AgentClass agent_class) {
    double score = 0.0;
    
    // Cover/positioning value based on agent class
    double center_x = width / 2.0;
    double center_y = height / 2.0;
    double dist_to_center = sqrt(pow(x - center_x, 2) + pow(y - center_y, 2));
    double max_dist = sqrt(pow(center_x, 2) + pow(center_y, 2));
    
    if (agent_class == SNIPER) {
        // Snipers prefer edges and corners for long-range safety
        double edge_distance = min({x, y, width-1-x, height-1-y});
        score += (1.0 - edge_distance / (min(width, height) / 2.0)) * 0.4;
    } else if (agent_class == BOMBER) {
        // Bombers prefer center for maximum throw coverage
        score += (1.0 - dist_to_center / max_dist) * 0.5;
    } else {
        // Others prefer moderate center control
        score += (1.0 - dist_to_center / max_dist) * 0.3;
    }
    
    // Enemy proximity evaluation
    if (!enemy_agents.empty()) {
        double min_enemy_dist = 999.0;
        for (const auto& enemy : enemy_agents) {
            double dist = manhattan_distance(x, y, enemy.x, enemy.y);
            min_enemy_dist = min(min_enemy_dist, dist);
        }
        
        // Different classes prefer different distances
        double optimal_distance = 3.0; // Default
        if (agent_class == SNIPER) optimal_distance = 5.0;
        if (agent_class == BERSERKER) optimal_distance = 2.0;
        if (agent_class == BOMBER) optimal_distance = 3.0;
        
        double distance_score = 1.0 - abs(min_enemy_dist - optimal_distance) / 10.0;
        score += max(0.0, distance_score) * 0.4;
    }
    
    // Ally coordination
    if (!my_agents.empty()) {
        double avg_ally_dist = 0.0;
        int ally_count = 0;
        for (const auto& ally : my_agents) {
            if (ally.x != x || ally.y != y) {
                avg_ally_dist += manhattan_distance(x, y, ally.x, ally.y);
                ally_count++;
            }
        }
        if (ally_count > 0) {
            avg_ally_dist /= ally_count;
            double optimal_ally_dist = (agent_class == SNIPER) ? 6.0 : 4.0;
            double spacing_score = 1.0 - abs(avg_ally_dist - optimal_ally_dist) / 8.0;
            score += max(0.0, spacing_score) * 0.2;
        }
    }
    
    return min(1.0, max(-1.0, score));
}

// Calculate territorial control score (inspired by Python version)
pair<int, int> calculate_controlled_area(const vector<AgentState>& my_agents, 
                                        const vector<AgentState>& enemy_agents,
                                        int width, int height) {
    int my_tiles = 0, enemy_tiles = 0;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Find minimum distance to my agents (with wetness penalty)
            double min_my_dist = 999.0;
            for (const auto& agent : my_agents) {
                if (agent.wetness < 100) { // Only alive agents
                    double base_dist = manhattan_distance(x, y, agent.x, agent.y);
                    double penalty_multiplier = (agent.wetness >= 50) ? 2.0 : 1.0; // Weakened agents have less control
                    double effective_dist = base_dist * penalty_multiplier;
                    min_my_dist = min(min_my_dist, effective_dist);
                }
            }
            
            // Find minimum distance to enemy agents (with wetness penalty)
            double min_enemy_dist = 999.0;
            for (const auto& agent : enemy_agents) {
                if (agent.wetness < 100) { // Only alive agents
                    double base_dist = manhattan_distance(x, y, agent.x, agent.y);
                    double penalty_multiplier = (agent.wetness >= 50) ? 2.0 : 1.0; // Weakened agents have less control
                    double effective_dist = base_dist * penalty_multiplier;
                    min_enemy_dist = min(min_enemy_dist, effective_dist);
                }
            }
            
            // Determine tile control
            if (min_my_dist < min_enemy_dist) {
                my_tiles++;
            } else if (min_enemy_dist < min_my_dist) {
                enemy_tiles++;
            }
            // Tied tiles don't count for either side
        }
    }
    
    return {my_tiles, enemy_tiles};
}

// Calculate tactical priority for an action (from tactical AI) with territorial control
double calculate_tactical_priority(const string& action_type, const AgentState& agent, 
                                 const AgentData& agent_data, int target_id, int target_x, int target_y,
                                 const vector<AgentState>& my_agents, const vector<AgentState>& enemy_agents,
                                 int width, int height) {
    
    AgentClass agent_class = determine_agent_class(agent_data);
    double tactical_component = 0.0;
    double positioning_component = 0.0;
    double survival_component = 0.0;
    double territorial_component = 0.0;
    
    // Tactical evaluation (50% weight - reduced to make room for territorial)
    if (action_type == "SHOOT" && agent.cooldown == 0) {
        tactical_component = 0.6; // High tactical value
        
        // Check if it's a kill shot
        for (const auto& enemy : enemy_agents) {
            if (enemy.agent_id == target_id) {
                int distance = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                int damage = calculate_shooting_damage(agent_data, enemy, distance);
                if (enemy.wetness + damage >= 100) {
                    tactical_component = 1.0; // Kill shot gets maximum priority
                }
                break;
            }
        }
    } else if (action_type == "THROW" && agent.cooldown == 0 && agent.splash_bombs > 0) {
        tactical_component = 0.4; // Moderate tactical value
        
        // Count potential splash targets
        int splash_targets = 0;
        for (const auto& enemy : enemy_agents) {
            int dist = manhattan_distance(target_x, target_y, enemy.x, enemy.y);
            if (dist <= 2) splash_targets++;
        }
        if (splash_targets > 1) tactical_component = 0.7; // Multi-target bonus
        
    } else if (action_type == "MOVE") {
        tactical_component = 0.1; // Low tactical value but strategic
    } else {
        tactical_component = -0.1; // Hunker down is defensive
    }
    
    // Positioning evaluation (15% weight)
    if (action_type == "MOVE") {
        positioning_component = evaluate_tile_strategic_value(target_x, target_y, width, height,
                                                            my_agents, enemy_agents, agent_class) * 0.15;
    } else {
        positioning_component = 0.0;
    }
    
    // Territorial control evaluation (20% weight - NEW!)
    if (action_type == "MOVE") {
        // Simulate the move and calculate territorial impact
        vector<AgentState> test_my_agents = my_agents;
        for (auto& test_agent : test_my_agents) {
            if (test_agent.agent_id == agent.agent_id) {
                test_agent.x = target_x;
                test_agent.y = target_y;
                break;
            }
        }
        
        // Calculate territorial control before and after move
        auto [my_tiles_before, enemy_tiles_before] = calculate_controlled_area(my_agents, enemy_agents, width, height);
        auto [my_tiles_after, enemy_tiles_after] = calculate_controlled_area(test_my_agents, enemy_agents, width, height);
        
        int territorial_gain = my_tiles_after - my_tiles_before;
        int territorial_loss = enemy_tiles_after - enemy_tiles_before;
        
        // Normalize territorial component (-1 to 1)
        double total_tiles = width * height;
        territorial_component = ((territorial_gain - territorial_loss) / total_tiles) * 0.2;
        territorial_component = max(-0.2, min(0.2, territorial_component));
        
    } else if (action_type == "SHOOT" || action_type == "THROW") {
        // Shooting/throwing doesn't directly change territory but weakening enemies helps
        territorial_component = 0.05; // Small territorial benefit from combat
    }
    
    // Survival component (15% weight - reduced)  
    survival_component = 0.15 * (100 - agent.wetness) / 100.0;
    
    // Final priority score: weighted sum normalized to [-1, 1]
    // 50% tactical + 15% positioning + 20% territorial + 15% survival = 100%
    double priority = tactical_component * 0.5 + positioning_component + territorial_component + survival_component;
    return max(-1.0, min(1.0, priority));
}

// Game simulation state
struct SimulationState {
    vector<AgentState> my_agents;
    vector<AgentState> enemy_agents;
    unordered_map<int, AgentData> agent_data;
    int width, height;
    
    // Smitsimax specific data
    vector<SmitsimaxNode*> current_nodes; // Current node for each agent
    vector<double> lowest_scores;          // For normalization
    vector<double> highest_scores;         // For normalization
    vector<double> scale_parameters;       // Normalization range
    
    SimulationState() = default;
    
    void reset_to_base_state(const vector<AgentState>& base_my, const vector<AgentState>& base_enemy) {
        my_agents = base_my;
        enemy_agents = base_enemy;
        
        // Reset cooldowns each turn
        for (auto& agent : my_agents) {
            if (agent.cooldown > 0) agent.cooldown--;
        }
        for (auto& agent : enemy_agents) {
            if (agent.cooldown > 0) agent.cooldown--;
        }
    }
};

// Apply action to simulation state
void apply_action(SimulationState& sim, int agent_index, bool is_my_agent) {
    vector<AgentState>& agents = is_my_agent ? sim.my_agents : sim.enemy_agents;
    vector<AgentState>& targets = is_my_agent ? sim.enemy_agents : sim.my_agents;
    
    if (agent_index >= agents.size()) return;
    
    AgentState& agent = agents[agent_index];
    SmitsimaxNode* node = sim.current_nodes[is_my_agent ? agent_index : agent_index + sim.my_agents.size()];
    
    if (node->action_type == "SHOOT" && agent.cooldown == 0) {
        // Find target and apply damage
        for (auto& target : targets) {
            if (target.agent_id == node->target_agent_id) {
                int distance = manhattan_distance(agent.x, agent.y, target.x, target.y);
                int damage = calculate_shooting_damage(sim.agent_data[agent.agent_id], target, distance);
                target.wetness += damage;
                agent.cooldown = sim.agent_data[agent.agent_id].shoot_cooldown;
                break;
            }
        }
    }
    else if (node->action_type == "MOVE") {
        // Move agent to new position
        if (node->target_x >= 0 && node->target_x < sim.width && 
            node->target_y >= 0 && node->target_y < sim.height) {
            agent.x = node->target_x;
            agent.y = node->target_y;
        }
    }
    else if (node->action_type == "THROW" && agent.cooldown == 0 && agent.splash_bombs > 0) {
        // Apply throw damage (3x3 area = radius 1)
        for (auto& target : targets) {
            int dist_to_throw = manhattan_distance(target.x, target.y, node->target_x, node->target_y);
            if (dist_to_throw <= 1) { // 3x3 splash area
                int damage = sim.agent_data[agent.agent_id].soaking_power / 2;
                target.wetness += damage;
            }
        }
        agent.splash_bombs--;
        agent.cooldown = sim.agent_data[agent.agent_id].shoot_cooldown;
    }
    // HUNKER_DOWN does nothing but is still a valid choice
}

// Generate all possible moves for an agent with tactical evaluation
vector<SmitsimaxNode*> create_tactical_moves(const AgentState& agent, const SimulationState& sim, bool is_my_agent) {
    vector<SmitsimaxNode*> moves;
    const AgentData& data = sim.agent_data.at(agent.agent_id);
    
    // Always include HUNKER_DOWN
    SmitsimaxNode* hunker = new SmitsimaxNode();
    hunker->action_type = "HUNKER_DOWN";
    hunker->tactical_priority = calculate_tactical_priority("HUNKER_DOWN", agent, data, -1, -1, -1,
                                                          sim.my_agents, sim.enemy_agents, sim.width, sim.height);
    moves.push_back(hunker);
    
    // SHOOTING options
    if (agent.cooldown == 0) {
        const vector<AgentState>& targets = is_my_agent ? sim.enemy_agents : sim.my_agents;
        for (const auto& target : targets) {
            if (target.wetness < 100) {
                int distance = manhattan_distance(agent.x, agent.y, target.x, target.y);
                if (distance <= data.optimal_range) {
                    SmitsimaxNode* shoot = new SmitsimaxNode();
                    shoot->action_type = "SHOOT";
                    shoot->target_agent_id = target.agent_id;
                    shoot->tactical_priority = calculate_tactical_priority("SHOOT", agent, data, target.agent_id, -1, -1,
                                                                         sim.my_agents, sim.enemy_agents, sim.width, sim.height);
                    moves.push_back(shoot);
                }
            }
        }
    }
    
    // MOVEMENT options - use tactical evaluation for best positions
    AgentClass agent_class = determine_agent_class(data);
    int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
    int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};
    
    for (int i = 0; i < 8; i++) {
        int nx = agent.x + dx[i];
        int ny = agent.y + dy[i];
        
        if (nx >= 0 && nx < sim.width && ny >= 0 && ny < sim.height) {
            // Check if position is free
            bool blocked = false;
            for (const auto& other : sim.my_agents) {
                if (other.x == nx && other.y == ny) {
                    blocked = true;
                    break;
                }
            }
            for (const auto& other : sim.enemy_agents) {
                if (other.x == nx && other.y == ny) {
                    blocked = true;
                    break;
                }
            }
            
            if (!blocked) {
                SmitsimaxNode* move = new SmitsimaxNode();
                move->action_type = "MOVE";
                move->target_x = nx;
                move->target_y = ny;
                move->tactical_priority = calculate_tactical_priority("MOVE", agent, data, -1, nx, ny,
                                                                    sim.my_agents, sim.enemy_agents, sim.width, sim.height);
                moves.push_back(move);
            }
        }
    }
    
    // THROWING options (for agents with bombs)
    if (agent.cooldown == 0 && agent.splash_bombs > 0) {
        const vector<AgentState>& targets = is_my_agent ? sim.enemy_agents : sim.my_agents;
        for (const auto& target : targets) {
            if (target.wetness < 100) {
                int distance = manhattan_distance(agent.x, agent.y, target.x, target.y);
                if (distance <= data.optimal_range * 2) {
                    SmitsimaxNode* throw_action = new SmitsimaxNode();
                    throw_action->action_type = "THROW";
                    throw_action->target_x = target.x;
                    throw_action->target_y = target.y;
                    throw_action->tactical_priority = calculate_tactical_priority("THROW", agent, data, -1, target.x, target.y,
                                                                                sim.my_agents, sim.enemy_agents, sim.width, sim.height);
                    moves.push_back(throw_action);
                }
            }
        }
    }
    
    return moves;
}

// Enhanced game state evaluation combining Smitsimax with tactical AI
double evaluate_enhanced_game_state(const SimulationState& sim, int agent_index, bool is_my_agent) {
    double score = 0.0;
    
    // Count live agents and health
    int my_live = 0, enemy_live = 0;
    int my_total_health = 0, enemy_total_health = 0;
    
    for (const auto& agent : sim.my_agents) {
        if (agent.wetness < 100) {
            my_live++;
            my_total_health += (100 - agent.wetness);
        }
    }
    
    for (const auto& agent : sim.enemy_agents) {
        if (agent.wetness < 100) {
            enemy_live++;
            enemy_total_health += (100 - agent.wetness);
        }
    }
    
    // Basic scoring
    if (is_my_agent) {
        score += (my_live - enemy_live) * 100;
        score += (my_total_health - enemy_total_health) * 0.5;
    } else {
        score += (enemy_live - my_live) * 100;
        score += (enemy_total_health - my_total_health) * 0.5;
    }
    
    // Territorial control scoring (NEW!)
    auto [my_controlled, enemy_controlled] = calculate_controlled_area(sim.my_agents, sim.enemy_agents, sim.width, sim.height);
    if (is_my_agent) {
        score += (my_controlled - enemy_controlled) * 2.0; // Territory advantage
    } else {
        score += (enemy_controlled - my_controlled) * 2.0; // Territory advantage
    }
    
    // Agent-specific scoring with tactical considerations
    const vector<AgentState>& agents = is_my_agent ? sim.my_agents : sim.enemy_agents;
    if (agent_index < agents.size()) {
        const AgentState& agent = agents[agent_index];
        if (agent.wetness < 100) {
            // Survival bonus
            score += (100 - agent.wetness) * 0.3;
            
            // Positional bonus using tactical evaluation
            AgentClass ac = determine_agent_class(sim.agent_data.at(agent.agent_id));
            double position_value = evaluate_tile_strategic_value(agent.x, agent.y, sim.width, sim.height,
                                                                sim.my_agents, sim.enemy_agents, ac);
            score += position_value * 20;
            
            // Combat readiness
            if (agent.cooldown == 0) score += 15;
            
            // Agent class specific bonuses
            const vector<AgentState>& targets = is_my_agent ? sim.enemy_agents : sim.my_agents;
            for (const auto& target : targets) {
                if (target.wetness < 100) {
                    int dist = manhattan_distance(agent.x, agent.y, target.x, target.y);
                    const AgentData& data = sim.agent_data.at(agent.agent_id);
                    
                    if (dist <= data.optimal_range) {
                        score += 10; // In optimal range
                        if (agent.cooldown == 0) {
                            int damage = calculate_shooting_damage(data, target, dist);
                            score += damage * 0.5;
                            if (target.wetness + damage >= 100) {
                                score += 50; // Kill shot opportunity
                            }
                        }
                    }
                }
            }
        }
    }
    
    return score;
}

// Pre-computed game state cache for instant decisions
struct GameStateKey {
    vector<pair<int, int>> my_positions;  // agent_id, x, y, wetness, cooldown
    vector<pair<int, int>> enemy_positions;
    
    bool operator==(const GameStateKey& other) const {
        return my_positions == other.my_positions && enemy_positions == other.enemy_positions;
    }
};

struct GameStateHash {
    size_t operator()(const GameStateKey& key) const {
        size_t hash = 0;
        for (auto& pos : key.my_positions) {
            hash ^= std::hash<int>{}(pos.first) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int>{}(pos.second) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        for (auto& pos : key.enemy_positions) {
            hash ^= std::hash<int>{}(pos.first) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int>{}(pos.second) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

struct PrecomputedMove {
    string action_type;
    int target_x = -1, target_y = -1;
    int target_agent_id = -1;
    double confidence_score = 0.0;
    string reasoning = "";
};

// Smitsimax search implementation with pre-computation cache
class MergedSmitsimaxSearch {
private:
    vector<SmitsimaxNode*> root_nodes;
    SimulationState sim;
    random_device rd;
    mt19937 gen;
    
    // Pre-computation cache
    unordered_map<GameStateKey, vector<PrecomputedMove>, GameStateHash> move_cache;
    bool cache_built = false;
    
public:
    MergedSmitsimaxSearch() : gen(rd()) {}
    
    ~MergedSmitsimaxSearch() {
        for (auto* root : root_nodes) {
            delete root;
            
        }
    }
    
    // Create game state key for caching
    GameStateKey create_state_key(const vector<AgentState>& my_agents, const vector<AgentState>& enemy_agents) {
        GameStateKey key;
        
        for (const auto& agent : my_agents) {
            key.my_positions.push_back({agent.agent_id, agent.x * 1000 + agent.y * 100 + agent.wetness + agent.cooldown});
        }
        
        for (const auto& agent : enemy_agents) {
            key.enemy_positions.push_back({agent.agent_id, agent.x * 1000 + agent.y * 100 + agent.wetness + agent.cooldown});
        }
        
        sort(key.my_positions.begin(), key.my_positions.end());
        sort(key.enemy_positions.begin(), key.enemy_positions.end());
        
        return key;
    }
    
    // Pre-compute all possible game scenarios
    void build_prediction_cache() {
        if (cache_built) return;
        
        cerr << "=== BUILDING PREDICTION CACHE ===" << endl;
        cerr << "Pre-computing all possible game scenarios..." << endl;
        
        auto start_time = chrono::high_resolution_clock::now();
        int scenarios_computed = 0;
        
        // Generate scenarios for different agent counts, positions, and health states
        for (int my_alive = 1; my_alive <= sim.my_agents.size(); my_alive++) {
            for (int enemy_alive = 1; enemy_alive <= sim.enemy_agents.size(); enemy_alive++) {
                
                // Sample different position combinations
                for (int pos_variant = 0; pos_variant < 20; pos_variant++) {
                    
                    // Create a test scenario
                    vector<AgentState> test_my = sim.my_agents;
                    vector<AgentState> test_enemy = sim.enemy_agents;
                    
                    // Randomize positions and health for variety
                    uniform_int_distribution<> pos_dist(0, min(sim.width-1, sim.height-1));
                    uniform_int_distribution<> health_dist(10, 90);
                    uniform_int_distribution<> cooldown_dist(0, 3);
                    
                    // Modify agent states for this scenario
                    for (int i = 0; i < test_my.size(); i++) {
                        if (i >= my_alive) {
                            test_my[i].wetness = 100; // Dead
                        } else {
                            test_my[i].x = pos_dist(gen);
                            test_my[i].y = pos_dist(gen);
                            test_my[i].wetness = health_dist(gen);
                            test_my[i].cooldown = cooldown_dist(gen);
                        }
                    }
                    
                    for (int i = 0; i < test_enemy.size(); i++) {
                        if (i >= enemy_alive) {
                            test_enemy[i].wetness = 100; // Dead
                        } else {
                            test_enemy[i].x = pos_dist(gen);
                            test_enemy[i].y = pos_dist(gen);
                            test_enemy[i].wetness = health_dist(gen);
                            test_enemy[i].cooldown = cooldown_dist(gen);
                        }
                    }
                    
                    // Run quick search for this scenario
                    SimulationState temp_sim = sim;
                    temp_sim.my_agents = test_my;
                    temp_sim.enemy_agents = test_enemy;
                    
                    // Quick tactical evaluation for each agent
                    vector<PrecomputedMove> scenario_moves;
                    for (int i = 0; i < test_my.size(); i++) {
                        PrecomputedMove best_move;
                        if (test_my[i].wetness < 100) {
                            // Agent is ALIVE - compute real move
                            best_move = compute_best_move_quick(test_my[i], temp_sim, i);
                        } else {
                            // Agent is DEAD - default action
                            best_move.action_type = "HUNKER_DOWN";
                            best_move.confidence_score = 0.0;
                            best_move.reasoning = "Agent dead";
                        }
                        scenario_moves.push_back(best_move);
                    }
                    
                    // Store in cache
                    GameStateKey key = create_state_key(test_my, test_enemy);
                    move_cache[key] = scenario_moves;
                    scenarios_computed++;
                    
                    // Time limit for cache building
                    auto current_time = chrono::high_resolution_clock::now();
                    auto elapsed = chrono::duration_cast<chrono::milliseconds>(current_time - start_time);
                    if (elapsed.count() > 2000) { // 2 second limit
                        cerr << "Cache building time limit reached" << endl;
                        goto cache_done;
                    }
                }
            }
        }
        
        cache_done:
        auto end_time = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time);
        
        cerr << "Pre-computation complete!" << endl;
        cerr << "Scenarios computed: " << scenarios_computed << endl;
        cerr << "Cache size: " << move_cache.size() << " entries" << endl;
        cerr << "Build time: " << duration.count() << "ms" << endl;
        
        cache_built = true;
    }
    
    // Quick move computation for fresh real-time decisions
    PrecomputedMove compute_best_move_quick(const AgentState& agent, const SimulationState& temp_sim, int agent_index) {
        PrecomputedMove move;
        const AgentData& data = temp_sim.agent_data.at(agent.agent_id);
        AgentClass agent_class = determine_agent_class(data);
        
        double best_score = -1000.0;
        
        cerr << "  Analyzing agent " << agent.agent_id << " (" << 
                (agent_class == SNIPER ? "SNIPER" : agent_class == BOMBER ? "BOMBER" : 
                 agent_class == GUNNER ? "GUNNER" : agent_class == ASSAULT ? "ASSAULT" : "BERSERKER")
                << ") at (" << agent.x << "," << agent.y << ") cooldown=" << agent.cooldown << endl;
        
        // PRIORITY 1: SNIPER long-range shooting (ALWAYS PRIORITIZE SHOOTING OVER MOVEMENT)
        if (agent_class == SNIPER && agent.cooldown == 0) {
            for (const auto& enemy : temp_sim.enemy_agents) {
                if (enemy.wetness < 100) {
                    int distance = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                    if (distance <= 6) { // SNIPER range
                        int damage = calculate_shooting_damage(data, enemy, distance);
                        if (damage > 0) { // Only shoot if damage > 0
                            double score = 2000.0 + damage * 25.0; // ULTRA HIGH SHOOTING PRIORITY
                            if (enemy.wetness + damage >= 100) score += 3000.0; // KILL SHOT MASSIVE BONUS
                            if (distance >= 4) score += 1000.0; // Long range bonus (SNIPER specialty)
                            if (distance == 6) score += 500.0; // Maximum range bonus
                            
                            cerr << "    SNIPER can shoot enemy " << enemy.agent_id << " at dist=" << distance 
                                 << " damage=" << damage << " score=" << score << endl;
                            
                            if (score > best_score) {
                                best_score = score;
                                move.action_type = "SHOOT";
                                move.target_agent_id = enemy.agent_id;
                                move.confidence_score = 1.0; // Max confidence for shooting
                                move.reasoning = "SNIPER long-range precision shot";
                            }
                        }
                    }
                }
            }
        }
        
        // PRIORITY 2: BOMBER splash bombing (FIND BEST BOMB LOCATION)
        if (agent_class == BOMBER && agent.cooldown == 0 && agent.splash_bombs > 0) {
            // Find best location for maximum splash damage
            pair<int, int> best_bomb_location = {-1, -1};
            double best_bomb_score = 0;
            
            // Check each enemy position as potential bomb target
            for (const auto& primary_enemy : temp_sim.enemy_agents) {
                if (primary_enemy.wetness < 100) {
                    int distance = manhattan_distance(agent.x, agent.y, primary_enemy.x, primary_enemy.y);
                    if (distance <= 4) { // BOMBER throw range (2x optimal range)
                        
                        // Count all enemies in splash radius around this target (3x3 = radius 1)
                        int splash_targets = 0;
                        int total_splash_damage = 0;
                        for (const auto& splash_enemy : temp_sim.enemy_agents) {
                            if (splash_enemy.wetness < 100) {
                                int splash_dist = manhattan_distance(primary_enemy.x, primary_enemy.y, 
                                                                   splash_enemy.x, splash_enemy.y);
                                if (splash_dist <= 1) { // 3x3 area = splash radius 1
                                    splash_targets++;
                                    total_splash_damage += data.soaking_power / 2;
                                }
                            }
                        }
                        
                        double bomb_score = 800.0 + total_splash_damage * 15.0; // BOMBER HUGE PRIORITY
                        if (splash_targets > 1) bomb_score += splash_targets * 1000.0; // MULTI-TARGET MASSIVE BONUS
                        
                        cerr << "    BOMBER can bomb (" << primary_enemy.x << "," << primary_enemy.y 
                             << ") targets=" << splash_targets << " damage=" << total_splash_damage 
                             << " score=" << bomb_score << endl;
                        
                        if (bomb_score > best_bomb_score) {
                            best_bomb_score = bomb_score;
                            best_bomb_location = {primary_enemy.x, primary_enemy.y};
                        }
                    }
                }
            }
            
            if (best_bomb_score > best_score) {
                best_score = best_bomb_score;
                move.action_type = "THROW";
                move.target_x = best_bomb_location.first;
                move.target_y = best_bomb_location.second;
                move.confidence_score = 1.0; // Max confidence for bombing
                move.reasoning = "BOMBER splash bombing cluster";
            }
        }
        
        // PRIORITY 3: Other agent shooting (ALWAYS PRIORITIZE SHOOTING)
        if (agent.cooldown == 0 && agent_class != SNIPER && agent_class != BOMBER) { 
            for (const auto& enemy : temp_sim.enemy_agents) {
                if (enemy.wetness < 100) {
                    int distance = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                    if (distance <= data.optimal_range) {
                        int damage = calculate_shooting_damage(data, enemy, distance);
                        if (damage > 0) { // Only shoot if damage > 0
                            double score = 1500.0 + damage * 20.0; // VERY HIGH SHOOTING PRIORITY
                            if (enemy.wetness + damage >= 100) score += 2500.0; // KILL SHOT HUGE BONUS
                            
                            // Agent class bonuses
                            if (agent_class == GUNNER && distance <= 2) score += 500.0;
                            if (agent_class == ASSAULT && distance <= 3) score += 600.0;
                            if (agent_class == BERSERKER && distance <= 2) score += 800.0;
                            
                            cerr << "    " << (agent_class == GUNNER ? "GUNNER" : agent_class == ASSAULT ? "ASSAULT" : "BERSERKER")
                                 << " can shoot enemy " << enemy.agent_id << " at dist=" << distance 
                                 << " damage=" << damage << " score=" << score << endl;
                            
                            if (score > best_score) {
                                best_score = score;
                                move.action_type = "SHOOT";
                                move.target_agent_id = enemy.agent_id;
                                move.confidence_score = 1.0; // Max confidence for shooting
                                move.reasoning = "Aggressive tactical shooting";
                            }
                        }
                    }
                }
            }
        }
        
        // PRIORITY 4: ASSAULT/GUNNER throwing (PRIORITIZE OVER MOVEMENT)
        if (agent.cooldown == 0 && agent.splash_bombs > 0 && 
            (agent_class == ASSAULT || agent_class == GUNNER)) {
            for (const auto& enemy : temp_sim.enemy_agents) {
                if (enemy.wetness < 100) {
                    int distance = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                    if (distance <= data.optimal_range * 2) {
                        
                        int splash_count = 0;
                        int total_damage = 0;
                        for (const auto& other_enemy : temp_sim.enemy_agents) {
                            if (other_enemy.wetness < 100) {
                                int splash_dist = manhattan_distance(enemy.x, enemy.y, other_enemy.x, other_enemy.y);
                                if (splash_dist <= 1) { // 3x3 area = splash radius 1
                                    splash_count++;
                                    total_damage += data.soaking_power / 2;
                                }
                            }
                        }
                        
                        double score = 600.0 + total_damage * 12.0; // THROWING PRIORITY
                        if (splash_count > 1) score += splash_count * 800.0; // MULTI-TARGET BONUS
                        
                        cerr << "    " << (agent_class == ASSAULT ? "ASSAULT" : "GUNNER")
                             << " can throw at (" << enemy.x << "," << enemy.y 
                             << ") targets=" << splash_count << " damage=" << total_damage 
                             << " score=" << score << endl;
                        
                        if (score > best_score) {
                            best_score = score;
                            move.action_type = "THROW";
                            move.target_x = enemy.x;
                            move.target_y = enemy.y;
                            move.confidence_score = 0.9; // High confidence for throwing
                            move.reasoning = "Tactical splash attack";
                        }
                    }
                }
            }
        }
        
        // PRIORITY 5: Movement (AGGRESSIVE COMBAT POSITIONING)
        int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};
        
        // Find closest enemy for targeting
        int closest_enemy_dist = 999;
        pair<int, int> closest_enemy_pos = {-1, -1};
        for (const auto& enemy : temp_sim.enemy_agents) {
            if (enemy.wetness < 100) {
                int dist = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                if (dist < closest_enemy_dist) {
                    closest_enemy_dist = dist;
                    closest_enemy_pos = {enemy.x, enemy.y};
                }
            }
        }
        
        for (int i = 0; i < 8; i++) {
            int nx = agent.x + dx[i];
            int ny = agent.y + dy[i];
            
            if (nx >= 0 && nx < temp_sim.width && ny >= 0 && ny < temp_sim.height) {
                // Check if position is free
                bool blocked = false;
                for (const auto& other : temp_sim.my_agents) {
                    if (other.x == nx && other.y == ny && other.agent_id != agent.agent_id) {
                        blocked = true;
                        break;
                    }
                }
                for (const auto& other : temp_sim.enemy_agents) {
                    if (other.x == nx && other.y == ny) {
                        blocked = true;
                        break;
                    }
                }
                
                if (!blocked) {
                    double strategic_score = evaluate_tile_strategic_value(nx, ny, temp_sim.width, temp_sim.height,
                                                                        temp_sim.my_agents, temp_sim.enemy_agents, agent_class);
                    
                    // AGGRESSIVE COMBAT POSITIONING - prioritize getting in range
                    double combat_positioning_score = 0.0;
                    
                    // Calculate improvement in combat potential
                    for (const auto& enemy : temp_sim.enemy_agents) {
                        if (enemy.wetness < 100) {
                            int current_dist = manhattan_distance(agent.x, agent.y, enemy.x, enemy.y);
                            int new_dist = manhattan_distance(nx, ny, enemy.x, enemy.y);
                            
                            // MASSIVE bonuses for getting into combat range
                            if (new_dist <= data.optimal_range && current_dist > data.optimal_range) {
                                combat_positioning_score += 800.0; // ENTERING SHOOTING RANGE = HUGE BONUS
                            }
                            if (new_dist <= data.optimal_range) {
                                combat_positioning_score += 400.0; // STAYING IN RANGE
                            }
                            
                            // Agent-specific range bonuses
                            if (agent_class == SNIPER) {
                                if (new_dist >= 4 && new_dist <= 6) combat_positioning_score += 600.0; // SNIPER optimal range
                                if (new_dist > current_dist && current_dist < 4) combat_positioning_score += 300.0; // Get sniper distance
                            }
                            if (agent_class == BOMBER && new_dist <= 4) {
                                combat_positioning_score += 500.0; // BOMBER in throw range
                                if (new_dist <= 2) combat_positioning_score += 300.0; // Optimal bomber range
                            }
                            if (agent_class == BERSERKER && new_dist <= 2) {
                                combat_positioning_score += 700.0; // BERSERKER close combat
                            }
                            
                            // Direction bonus - move toward closest enemy
                            if (closest_enemy_pos.first != -1) {
                                int current_approach = manhattan_distance(agent.x, agent.y, closest_enemy_pos.first, closest_enemy_pos.second);
                                int new_approach = manhattan_distance(nx, ny, closest_enemy_pos.first, closest_enemy_pos.second);
                                if (new_approach < current_approach) {
                                    combat_positioning_score += 200.0; // APPROACHING ENEMY
                                }
                            }
                        }
                    }
                    
                    // Combined scoring - combat positioning should compete with low-tier combat actions
                    double combined_score = strategic_score * 50.0 + combat_positioning_score;
                    
                    // CRITICAL: Movement MUST never beat shooting/bombing - cap at 1400 max
                    if (combined_score > 1400.0) {
                        combined_score = 1400.0; // Ensure shooting (1500+) always beats movement
                    }
                    
                    cerr << "      Move to (" << nx << "," << ny << ") strategic=" << strategic_score 
                         << " combat_pos=" << combat_positioning_score << " total=" << combined_score << endl;
                    
                    if (combined_score > best_score) {
                        best_score = combined_score;
                        move.action_type = "MOVE";
                        move.target_x = nx;
                        move.target_y = ny;
                        move.confidence_score = min(1.0, combined_score / 1400.0); // Scale to movement cap
                        move.reasoning = (combat_positioning_score >= 400.0) ? "Aggressive approach for combat" : "Strategic positioning";
                    }
                }
            }
        }
        
        // Default to hunker down if no good options
        if (move.action_type.empty()) {
            move.action_type = "HUNKER_DOWN";
            move.confidence_score = 0.1;
            move.reasoning = "Safe defensive option";
        }
        
        cerr << "    DECISION: " << move.action_type;
        if (move.action_type == "SHOOT") cerr << " target:" << move.target_agent_id;
        if (move.action_type == "MOVE") cerr << " to:(" << move.target_x << "," << move.target_y << ")";
        if (move.action_type == "THROW") cerr << " at:(" << move.target_x << "," << move.target_y << ")";
        cerr << " confidence:" << move.confidence_score << endl;
        
        return move;
    }
    
    // Fast lookup for pre-computed moves - ALWAYS COMPUTE FRESH FOR ACCURACY
    vector<PrecomputedMove> get_cached_moves(const vector<AgentState>& my_agents, const vector<AgentState>& enemy_agents) {
        cerr << "COMPUTING FRESH MOVES: Analyzing current battlefield state" << endl;
        
        // ALWAYS compute fresh moves for accuracy - no bad cache matches
        vector<PrecomputedMove> moves;
        for (int i = 0; i < my_agents.size(); i++) {
            PrecomputedMove move;
            if (my_agents[i].wetness < 100) {
                // Agent is ALIVE - compute best move with current state
                SimulationState fresh_sim = sim;
                fresh_sim.my_agents = my_agents;
                fresh_sim.enemy_agents = enemy_agents;
                move = compute_best_move_quick(my_agents[i], fresh_sim, i);
                cerr << "Agent " << my_agents[i].agent_id << " ALIVE: Computed " << move.action_type 
                     << " (confidence:" << move.confidence_score << ")" << endl;
            } else {
                // Agent is DEAD
                move.action_type = "HUNKER_DOWN";
                move.confidence_score = 0.0;
                move.reasoning = "Agent dead";
                cerr << "Agent " << my_agents[i].agent_id << " DEAD: Default HUNKER_DOWN" << endl;
            }
            moves.push_back(move);
        }
        
        return moves;
    }
    
    // Calculate similarity between game scenarios
    double calculate_scenario_similarity(const GameStateKey& key1, const GameStateKey& key2) {
        if (key1.my_positions.size() != key2.my_positions.size() ||
            key1.enemy_positions.size() != key2.enemy_positions.size()) {
            return 0.0;
        }
        
        double similarity = 0.0;
        int comparisons = 0;
        
        // Compare agent positions and states
        for (int i = 0; i < key1.my_positions.size(); i++) {
            int pos1 = key1.my_positions[i].second;
            int pos2 = key2.my_positions[i].second;
            
            int x1 = pos1 / 1000, y1 = (pos1 % 1000) / 100, h1 = (pos1 % 100) / 10;
            int x2 = pos2 / 1000, y2 = (pos2 % 1000) / 100, h2 = (pos2 % 100) / 10;
            
            double pos_sim = 1.0 - (manhattan_distance(x1, y1, x2, y2) / 20.0);
            double health_sim = 1.0 - abs(h1 - h2) / 10.0;
            
            similarity += max(0.0, (pos_sim + health_sim) / 2.0);
            comparisons++;
        }
        
        return comparisons > 0 ? similarity / comparisons : 0.0;
    }
    
    void initialize(const vector<AgentState>& my_agents, const vector<AgentState>& enemy_agents,
                   const unordered_map<int, AgentData>& agent_data, int width, int height) {
        
        // Clean up previous trees
        for (auto* root : root_nodes) {
            delete root;
        }
        root_nodes.clear();
        
        // Setup simulation state
        sim.my_agents = my_agents;
        sim.enemy_agents = enemy_agents;
        sim.agent_data = agent_data;
        sim.width = width;
        sim.height = height;
        
        // Create root nodes for each agent
        int total_agents = my_agents.size() + enemy_agents.size();
        root_nodes.resize(total_agents);
        sim.current_nodes.resize(total_agents);
        sim.lowest_scores.resize(total_agents, 0.0);
        sim.highest_scores.resize(total_agents, 0.0);
        sim.scale_parameters.resize(total_agents, 1.0);
        
        for (int i = 0; i < total_agents; i++) {
            root_nodes[i] = new SmitsimaxNode();
            sim.current_nodes[i] = root_nodes[i];
        }
    }
    
    SmitsimaxNode* select_child_ucb(SmitsimaxNode* node, int agent_index) {
        if (node->children.empty()) return nullptr;
        if (node->visits < MIN_RANDOM_VISITS) {
            // Random selection for first few visits to avoid resonance
            uniform_int_distribution<> dis(0, node->children.size() - 1);
            return node->children[dis(gen)];
        }
        
        // UCB selection with tactical priority integration
        SmitsimaxNode* best_child = nullptr;
        double best_ucb = -numeric_limits<double>::infinity();
        
        for (auto* child : node->children) {
            if (child->visits == 0) {
                // Unvisited nodes get infinite priority, but prefer tactically sound moves
                if (best_child == nullptr || child->tactical_priority > best_child->tactical_priority) {
                    best_child = child;
                }
                continue;
            }
            
            double avg_score = child->get_average_score();
            double normalized_score = avg_score / (child->visits * sim.scale_parameters[agent_index]);
            double exploration = EXPLORATION_PARAM * sqrt(log(node->visits)) * (1.0 / sqrt(child->visits));
            double tactical_bonus = child->tactical_priority * 0.3; // Blend tactical evaluation
            double ucb = normalized_score + exploration + tactical_bonus;
            
            if (ucb > best_ucb) {
                best_ucb = ucb;
                best_child = child;
            }
        }
        
        return best_child;
    }
    
    void expand_node(SmitsimaxNode* node, int agent_index) {
        if (!node->children.empty()) return;
        
        bool is_my_agent = agent_index < sim.my_agents.size();
        const vector<AgentState>& agents = is_my_agent ? sim.my_agents : sim.enemy_agents;
        int actual_index = is_my_agent ? agent_index : agent_index - sim.my_agents.size();
        
        if (actual_index < agents.size()) {
            vector<SmitsimaxNode*> moves = create_tactical_moves(agents[actual_index], sim, is_my_agent);
            for (auto* move : moves) {
                move->parent = node;
                node->children.push_back(move);
            }
        }
    }
    
    void backpropagate(SmitsimaxNode* node, double score, int agent_index) {
        while (node != nullptr) {
            node->visits++;
            node->total_score += score;
            
            // Update normalization parameters
            if (score < sim.lowest_scores[agent_index]) {
                sim.lowest_scores[agent_index] = score;
            }
            if (score > sim.highest_scores[agent_index]) {
                sim.highest_scores[agent_index] = score;
            }
            
            double range = sim.highest_scores[agent_index] - sim.lowest_scores[agent_index];
            sim.scale_parameters[agent_index] = max(1.0, range);
            
            node = node->parent;
        }
    }
    
    vector<SmitsimaxNode*> search(int max_time_ms = MAX_SIMULATION_TIME) {
        cerr << "=== USING PRE-COMPUTED CACHE SYSTEM ===" << endl;
        
        // Calculate current territorial control
        auto [my_controlled, enemy_controlled] = calculate_controlled_area(sim.my_agents, sim.enemy_agents, sim.width, sim.height);
        double total_tiles = sim.width * sim.height;
        double my_control_percent = (my_controlled / total_tiles) * 100.0;
        double enemy_control_percent = (enemy_controlled / total_tiles) * 100.0;
        
        cerr << "TERRITORIAL CONTROL: My=" << my_controlled << "(" << my_control_percent << "%) "
             << "Enemy=" << enemy_controlled << "(" << enemy_control_percent << "%) "
             << "Neutral=" << (total_tiles - my_controlled - enemy_controlled) << endl;
        
        // Try cache lookup first
        vector<PrecomputedMove> cached_moves = get_cached_moves(sim.my_agents, sim.enemy_agents);
        
        // Convert cached moves to SmitsimaxNode format
        vector<SmitsimaxNode*> result_moves;
        
        for (int i = 0; i < sim.my_agents.size(); i++) {
            SmitsimaxNode* move_node = new SmitsimaxNode();
            
            if (i < cached_moves.size()) {
                PrecomputedMove& cached = cached_moves[i];
                move_node->action_type = cached.action_type;
                move_node->target_x = cached.target_x;
                move_node->target_y = cached.target_y;
                move_node->target_agent_id = cached.target_agent_id;
                move_node->tactical_priority = cached.confidence_score;
                move_node->visits = 100; // High confidence indicator
                move_node->total_score = cached.confidence_score * 100;
                
                cerr << "Agent " << sim.my_agents[i].agent_id << " CACHED: " << cached.action_type;
                if (cached.action_type == "SHOOT") cerr << " target:" << cached.target_agent_id;
                if (cached.action_type == "MOVE") cerr << " to:(" << cached.target_x << "," << cached.target_y << ")";
                cerr << " (confidence:" << cached.confidence_score << " reason:" << cached.reasoning << ")" << endl;
            } else {
                // Fallback
                move_node->action_type = "HUNKER_DOWN";
                move_node->tactical_priority = 0.1;
                move_node->visits = 1;
                cerr << "Agent " << sim.my_agents[i].agent_id << " FALLBACK: HUNKER_DOWN" << endl;
            }
            
            result_moves.push_back(move_node);
        }
        
        cerr << "=== INSTANT CACHE LOOKUP COMPLETE ===" << endl;
        return result_moves;
    }
    
    // Original search method renamed for backup use
    vector<SmitsimaxNode*> search_original(int max_time_ms = MAX_SIMULATION_TIME) {
        auto start_time = chrono::high_resolution_clock::now();
        
        cerr << "=== MERGED SMITSIMAX + TACTICAL SEARCH ===" << endl;
        cerr << "Searching with " << root_nodes.size() << " agent trees (enhanced tactical evaluation)" << endl;
        
        int iterations = 0;
        
        while (true) {
            auto current_time = chrono::high_resolution_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(current_time - start_time);
            if (elapsed.count() >= max_time_ms) {
                cerr << "Search timeout reached at " << elapsed.count() << "ms" << endl;
                break;
            }
            
            // Safety check to prevent infinite loops
            if (iterations > 10000) {
                cerr << "Maximum iterations reached: " << iterations << endl;
                break;
            }
            
            // Reset simulation to base state
            sim.reset_to_base_state(sim.my_agents, sim.enemy_agents);
            
            // Selection and simulation phase
            for (int depth = 0; depth < MAX_SEARCH_DEPTH; depth++) {
                // Process each agent
                for (int agent_idx = 0; agent_idx < root_nodes.size(); agent_idx++) {
                    SmitsimaxNode* current = sim.current_nodes[agent_idx];
                    
                    // Expand if needed
                    if (current->visits == 1) {
                        expand_node(current, agent_idx);
                    }
                    
                    // Select child
                    if (!current->children.empty()) {
                        SmitsimaxNode* selected = select_child_ucb(current, agent_idx);
                        if (selected) {
                            selected->visits++;
                            sim.current_nodes[agent_idx] = selected;
                            
                            // Apply the move
                            bool is_my_agent = agent_idx < sim.my_agents.size();
                            int actual_index = is_my_agent ? agent_idx : agent_idx - sim.my_agents.size();
                            apply_action(sim, actual_index, is_my_agent);
                        }
                    }
                }
            }
            
            // Enhanced evaluation and backpropagation
            for (int agent_idx = 0; agent_idx < root_nodes.size(); agent_idx++) {
                bool is_my_agent = agent_idx < sim.my_agents.size();
                int actual_index = is_my_agent ? agent_idx : agent_idx - sim.my_agents.size();
                
                double score = evaluate_enhanced_game_state(sim, actual_index, is_my_agent);
                backpropagate(sim.current_nodes[agent_idx], score, agent_idx);
            }
            
            // Reset current nodes to roots for next iteration
            for (int i = 0; i < root_nodes.size(); i++) {
                sim.current_nodes[i] = root_nodes[i];
            }
            
            iterations++;
        }
        
        cerr << "Merged search completed " << iterations << " iterations in " 
             << max_time_ms << "ms" << endl;
        
        // Select best moves using combined scoring
        vector<SmitsimaxNode*> best_moves;
        for (int i = 0; i < sim.my_agents.size(); i++) {
            SmitsimaxNode* root = root_nodes[i];
            SmitsimaxNode* best_child = nullptr;
            double best_combined_score = -numeric_limits<double>::infinity();
            
            const AgentState& agent = sim.my_agents[i];
            AgentClass ac = sim.agent_data[agent.agent_id].agent_class;
            string class_name = (ac == SNIPER ? "SNIPER" : ac == BOMBER ? "BOMBER" : 
                               ac == BERSERKER ? "BERSERKER" : ac == ASSAULT ? "ASSAULT" : "GUNNER");
            
            cerr << "Agent " << agent.agent_id << " (" << class_name << ") merged analysis:" << endl;
            
            for (auto* child : root->children) {
                double smitsimax_score = child->get_average_score();
                double tactical_score = child->tactical_priority;
                double visit_confidence = min(1.0, child->visits / 30.0);
                
                // Combined score: 60% Smitsimax + 40% Tactical Priority
                double combined_score = (smitsimax_score * 0.6 + tactical_score * 40 * 0.4) * visit_confidence;
                
                cerr << "  " << child->action_type;
                if (child->action_type == "SHOOT") cerr << " target:" << child->target_agent_id;
                if (child->action_type == "MOVE") cerr << " to:(" << child->target_x << "," << child->target_y << ")";
                if (child->action_type == "THROW") cerr << " at:(" << child->target_x << "," << child->target_y << ")";
                cerr << " -> visits:" << child->visits << " smitsimax:" << smitsimax_score 
                     << " tactical:" << tactical_score << " combined:" << combined_score << endl;
                
                if (combined_score > best_combined_score) {
                    best_combined_score = combined_score;
                    best_child = child;
                }
            }
            
            best_moves.push_back(best_child);
            
            if (best_child) {
                cerr << "*** BEST MERGED DECISION: " << best_child->action_type 
                     << " (combined_score:" << best_combined_score << ") ***" << endl;
            } else {
                cerr << "*** NO MOVE SELECTED - DEFAULTING TO HUNKER_DOWN ***" << endl;
            }
        }
        
        // Opponent prediction analysis
        cerr << endl << "=== OPPONENT PREDICTION ANALYSIS ===" << endl;
        for (int i = sim.my_agents.size(); i < root_nodes.size(); i++) {
            SmitsimaxNode* enemy_root = root_nodes[i];
            SmitsimaxNode* predicted_enemy_move = nullptr;
            double best_enemy_score = -numeric_limits<double>::infinity();
            
            int enemy_index = i - sim.my_agents.size();
            if (enemy_index < sim.enemy_agents.size()) {
                cerr << "Enemy " << sim.enemy_agents[enemy_index].agent_id << " prediction:" << endl;
                
                for (auto* child : enemy_root->children) {
                    double avg_score = child->get_average_score();
                    cerr << "  Likely: " << child->action_type;
                    if (child->action_type == "SHOOT") cerr << " target:" << child->target_agent_id;
                    if (child->action_type == "MOVE") cerr << " to:(" << child->target_x << "," << child->target_y << ")";
                    cerr << " (visits:" << child->visits << " score:" << avg_score << ")" << endl;
                    
                    if (avg_score > best_enemy_score) {
                        best_enemy_score = avg_score;
                        predicted_enemy_move = child;
                    }
                }
                
                if (predicted_enemy_move) {
                    cerr << "  *** MOST LIKELY: " << predicted_enemy_move->action_type;
                    if (predicted_enemy_move->action_type == "SHOOT") {
                        cerr << " targeting agent " << predicted_enemy_move->target_agent_id;
                    }
                    cerr << " ***" << endl;
                }
            }
        }
        
        cerr << "=== MERGED SEARCH END ===" << endl;
        
        return best_moves;
    }
};

int main() {
    int my_id;
    cin >> my_id;
    cin.ignore();
    
    int agent_data_count;
    cin >> agent_data_count;
    cin.ignore();
    
    unordered_map<int, AgentData> all_agents_data;
    vector<int> my_agent_ids;
    vector<int> enemy_agent_ids;
    
    for (int i = 0; i < agent_data_count; i++) {
        AgentData agent;
        cin >> agent.agent_id >> agent.player >> agent.shoot_cooldown 
            >> agent.optimal_range >> agent.soaking_power >> agent.splash_bombs;
        cin.ignore();
        
        agent.agent_class = determine_agent_class(agent);
        all_agents_data[agent.agent_id] = agent;
        
        if (agent.player == my_id) {
            my_agent_ids.push_back(agent.agent_id);
        } else {
            enemy_agent_ids.push_back(agent.agent_id);
        }
    }
    
    int width, height;
    cin >> width >> height;
    cin.ignore();
    
    // Skip map data for now (can be added later if needed)
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int x, y, tile_type;
            cin >> x >> y >> tile_type;
            cin.ignore();
        }
    }
    
    MergedSmitsimaxSearch search;
    
    cerr << "=== INITIALIZING PRE-COMPUTATION SYSTEM ===" << endl;
    cerr << "Building prediction cache before game starts..." << endl;
    
    // Initialize search with dummy data to build cache
    vector<AgentState> initial_my, initial_enemy;
    for (int id : my_agent_ids) {
        AgentState agent;
        agent.agent_id = id;
        agent.x = 0; agent.y = 0; agent.cooldown = 0; agent.splash_bombs = all_agents_data[id].splash_bombs;
        agent.wetness = 50; // Mid health for cache building
        initial_my.push_back(agent);
    }
    for (int id : enemy_agent_ids) {
        AgentState agent;
        agent.agent_id = id;
        agent.x = width-1; agent.y = height-1; agent.cooldown = 0; agent.splash_bombs = 1;
        agent.wetness = 50;
        initial_enemy.push_back(agent);
    }
    
    search.initialize(initial_my, initial_enemy, all_agents_data, width, height);
    search.build_prediction_cache(); // Pre-compute everything!
    
    cerr << "=== CACHE READY - STARTING REAL-TIME GAME ===" << endl;
    
    while (true) {
        auto turn_start = chrono::high_resolution_clock::now();
        
        int agent_count;
        cin >> agent_count;
        if (cin.fail()) {
            cerr << "ERROR: Failed to read agent_count!" << endl;
            break;
        }
        cin.ignore();
        
        cerr << "=== TURN START: Reading " << agent_count << " agents ===" << endl;
        
        vector<AgentState> my_current_agents;
        vector<AgentState> enemy_current_agents;
        
        for (int i = 0; i < agent_count; i++) {
            AgentState agent;
            cin >> agent.agent_id >> agent.x >> agent.y
                >> agent.cooldown >> agent.splash_bombs >> agent.wetness;
            cin.ignore();
            
            bool is_my_agent = find(my_agent_ids.begin(), my_agent_ids.end(), agent.agent_id) != my_agent_ids.end();
            
            if (is_my_agent) {
                my_current_agents.push_back(agent);
            } else {
                enemy_current_agents.push_back(agent);
            }
        }
        
        int my_agent_count;
        cin >> my_agent_count;
        if (cin.fail()) {
            cerr << "ERROR: Failed to read my_agent_count!" << endl;
            break;
        }
        cin.ignore();
        
        cerr << "=== TURN INFO ===" << endl;
        cerr << "Game expects " << my_agent_count << " action lines from me" << endl;
        cerr << "I have " << my_current_agents.size() << " live agents" << endl;
        
        cerr << "=== MERGED SMITSIMAX + TACTICAL AI ===" << endl;
        cerr << "Expected my_agent_count: " << my_agent_count << endl;
        cerr << "Actual my_current_agents.size(): " << my_current_agents.size() << endl;
        cerr << "My agents: " << my_current_agents.size() 
             << ", Enemy agents: " << enemy_current_agents.size() << endl;
        
        cerr << "My agent IDs: ";
        for (int id : my_agent_ids) cerr << id << " ";
        cerr << endl;
        
        cerr << "Live agent IDs: ";
        for (const auto& agent : my_current_agents) cerr << agent.agent_id << " ";
        cerr << endl;
        
        // Print current state with detailed info
        cerr << "Current battlefield:" << endl;
        for (const auto& agent : my_current_agents) {
            AgentClass ac = all_agents_data[agent.agent_id].agent_class;
            string class_name = (ac == SNIPER ? "SNIPER" : ac == BOMBER ? "BOMBER" : 
                               ac == BERSERKER ? "BERSERKER" : ac == ASSAULT ? "ASSAULT" : "GUNNER");
            cerr << "  My " << class_name << " " << agent.agent_id << ": pos(" << agent.x << "," << agent.y 
                 << ") cooldown=" << agent.cooldown << " wetness=" << agent.wetness 
                 << " bombs=" << agent.splash_bombs << endl;
        }
        for (const auto& agent : enemy_current_agents) {
            cerr << "  Enemy " << agent.agent_id << ": pos(" << agent.x << "," << agent.y 
                 << ") cooldown=" << agent.cooldown << " wetness=" << agent.wetness << endl;
        }
        
        // Initialize and run INSTANT cache lookup (no real-time search needed!)
        cerr << "Updating search state for turn with " << my_current_agents.size() << " my agents, " 
             << enemy_current_agents.size() << " enemy agents" << endl;
        
        search.initialize(my_current_agents, enemy_current_agents, all_agents_data, width, height);
        
        cerr << "Running INSTANT cache lookup..." << endl;
        vector<SmitsimaxNode*> best_moves;
        
        try {
            best_moves = search.search(); // Uses cache now!
            cerr << "Cache lookup completed instantly, got " << best_moves.size() << " moves" << endl;
        } catch (...) {
            cerr << "Cache lookup failed! Using emergency defaults." << endl;
            // Create default moves for all agents
            for (int i = 0; i < my_current_agents.size(); i++) {
                best_moves.push_back(nullptr);
            }
        }
        
        // Output actions - SIMPLE FORMAT ONLY
        cerr << endl << "=== GENERATING SIMPLE OUTPUT FORMAT ===" << endl;
        
        for (int i = 0; i < my_agent_count; i++) {
            string final_action;
            
            if (i < my_current_agents.size()) {
                // Live agent - use AI decision
                int agent_id = my_current_agents[i].agent_id;
                
                if (i < best_moves.size() && best_moves[i]) {
                    SmitsimaxNode* move = best_moves[i];
                    
                    if (move->action_type == "SHOOT") {
                        final_action = to_string(agent_id) + ";SHOOT " + to_string(move->target_agent_id) + "; HUNKER_DOWN";
                        cerr << "Agent " << agent_id << " -> SHOOT " << move->target_agent_id << endl;
                    } else if (move->action_type == "MOVE") {
                        final_action = to_string(agent_id) + ";MOVE " + to_string(move->target_x) + " " + to_string(move->target_y) + "; HUNKER_DOWN";
                        cerr << "Agent " << agent_id << " -> MOVE " << move->target_x << " " << move->target_y << endl;
                    } else if (move->action_type == "THROW") {
                        final_action = to_string(agent_id) + ";THROW " + to_string(move->target_x) + " " + to_string(move->target_y) + "; HUNKER_DOWN";
                        cerr << "Agent " << agent_id << " -> THROW " << move->target_x << " " << move->target_y << endl;
                    } else {
                        final_action = to_string(agent_id) + ";HUNKER_DOWN; HUNKER_DOWN";
                        cerr << "Agent " << agent_id << " -> HUNKER_DOWN" << endl;
                    }
                } else {
                    final_action = to_string(agent_id) + ";HUNKER_DOWN; HUNKER_DOWN";
                    cerr << "Agent " << agent_id << " -> DEFAULT HUNKER_DOWN" << endl;
                }
            } else {
                // Dead agent - use default ID
                int default_id = (i < my_agent_ids.size()) ? my_agent_ids[i] : my_agent_ids[0];
                final_action = to_string(default_id) + ";HUNKER_DOWN; HUNKER_DOWN";
                cerr << "Dead agent slot " << i << " -> Agent " << default_id << " HUNKER_DOWN" << endl;
            }
            
            cout << final_action << endl;
            cerr << "SENT TO GAME: " << final_action << endl;
        }
        
        // CRITICAL: Ensure all output is flushed immediately
        cout.flush();
        cerr.flush();
        auto turn_end = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(turn_end - turn_start);
        cerr << "INSTANT cached turn time: " << duration.count() << "ms (cache system)" << endl;
        cerr << "========================================" << endl << endl;
    }
    
    return 0;
}
