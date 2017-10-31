#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "poet.h"
#include "poet_constants.h"
#include "poet_math.h"

#ifdef FIXED_POINT
#pragma message "Compiling fixed point version"
#else
#pragma message "Compiling floating point version"
#endif

/*
##################################################
###########  POET INTERNAL DATATYPES #############
##################################################
*/

// Represents state of kalman filter used in estimate_workload
typedef struct {
  real_t x_hat_minus;
  real_t x_hat;
  real_t p_minus;
  real_t h;
  real_t k;
  real_t p;
} filter_state;

// Container for old speedup/powerup values and old errors
typedef struct {
  real_t u;
  real_t uo;
  real_t uoo;
  real_t e;
  real_t eo;
  real_t umin;
  real_t umax;
} calc_xup_state;

// Container for log records
typedef struct {
  unsigned long tag;
  poet_tradeoff_type_t constraint;
  real_t act_rate;
  real_t act_power;
  filter_state pfs;
  calc_xup_state scs;
  filter_state cfs;
  calc_xup_state pcs;
  real_t time_workload;
  real_t energy_workload;
  int lower_id;
  int upper_id;
  int low_state_iters;
  unsigned long long idle_ns;
} poet_record;

struct poet_internal_state {
  // log file and log buffer
  FILE * log_file;
  unsigned int buffer_depth;
  poet_record * lb;

  // constraint type
  poet_tradeoff_type_t constraint;
  real_t constraint_goal;

  // performance filter state
  filter_state pfs;

  // cost filter state
  filter_state cfs;

  // speedup calculation state
  calc_xup_state scs;

  // powerup calculation state
  calc_xup_state pcs;

  // general
  int current_action;

  int lower_id;
  int upper_id;
  unsigned int last_id;
  int low_state_iters;
  unsigned int period;
  unsigned long long idle_ns;
  real_t cost_estimate;
  real_t cost_xup_estimate;

  unsigned int num_system_states;
  poet_apply_func apply;
  poet_control_state_t * control_states;
  void * apply_states;
  // track if we've ever applied a state
  // (assumption of initial state could be incorrect)
  unsigned int is_first_apply;
};

/*
##################################################
###########  POET FUNCTION DEFINITIONS  ##########
##################################################
*/

// Allocates and initializes a new poet state variable
poet_state * poet_init(real_t goal,
                       poet_tradeoff_type_t constraint,
                       unsigned int num_system_states,
                       poet_control_state_t * control_states,
                       void * apply_states,
                       poet_apply_func apply,
                       poet_curr_state_func current,
                       unsigned int period,
                       unsigned int buffer_depth,
                       const char * log_filename) {
  unsigned int i;

  if (goal <= R_ZERO || num_system_states == 0 || control_states == NULL || period == 0 ||
      (buffer_depth == 0 && log_filename != NULL)) {
    errno = EINVAL;
    return NULL;
  }

  // Allocate memory for state struct
  poet_state * state = (poet_state *) malloc(sizeof(struct poet_internal_state));
  if (state == NULL) {
    return NULL;
  }

  // Remember constraint type
  state->constraint = constraint;

  // Remember the constraint goal
  state->constraint_goal = goal;

  // Remember the period
  state->period = period;

  // Allocate memory for log buffer
  state->buffer_depth = buffer_depth;
  if (buffer_depth > 0) {
    state->lb = malloc(buffer_depth * sizeof(poet_record));
    if (state->lb == NULL) {
      free(state);
      return NULL;
    }
  } else {
    state->lb = NULL;
  }

  // Open log file
  if (log_filename == NULL) {
    state->log_file = NULL;
  } else {
    state->log_file = fopen(log_filename, "w");
    if (state->log_file == NULL) {
      perror(log_filename);
      free(state->lb);
      free(state);
      return NULL;
    }
    fprintf(state->log_file,
            "%16s %16s "
            "%16s %16s %16s %16s %16s %16s %16s %16s %16s "
            "%16s %16s %16s %16s %16s %16s %16s %16s %16s "
            "%16s %16s %16s %16s %16s %16s\n",
            "TAG", "CONSTRAINT",
            "ACTUAL_RATE", "P_X_HAT_MINUS", "P_X_HAT", "P_P_MINUS", "P_H", "P_K", "P_P", "P_SPEEDUP", "P_ERROR",
            "ACTUAL_POWER", "C_X_HAT_MINUS", "C_X_HAT", "C_P_MINUS", "C_H", "C_K", "C_P", "C_POWERUP", "C_ERROR",
            "TIME_WORKLOAD", "ENERGY_WORKLOAD", "LOWER_ID", "UPPER_ID", "LOW_STATE_ITERS", "IDLE_NS");
  }

  // initialize variables used in the performance filter
  state->pfs.x_hat_minus = X_HAT_MINUS_START;
  state->pfs.x_hat = X_HAT_START;
  state->pfs.p_minus = P_MINUS_START;
  state->pfs.h = H_START;
  state->pfs.k = K_START;
  state->pfs.p = P_START;

  // initialize variables used in the cost filter
  state->cfs.x_hat_minus = X_HAT_MINUS_START;
  state->cfs.x_hat = X_HAT_START;
  state->cfs.p_minus = P_MINUS_START;
  state->cfs.h = H_START;
  state->cfs.k = K_START;
  state->cfs.p = P_START;

  // initialize general poet variables
  state->current_action = CURRENT_ACTION_START;
  state->num_system_states = num_system_states;
  state->apply = apply;
  state->control_states = control_states;
  state->apply_states = apply_states; // allowed to be NULL
  state->is_first_apply = 1;

  state->upper_id = -1;
  state->lower_id = -1;

  // try to get the initial system state
  if (current == NULL || current(state->apply_states, state->num_system_states, &state->last_id)) {
    // default to the highest state id
    state->last_id = state->num_system_states - 1;
  }

  // initialize variables used for calculating speedup
  state->scs.u = state->control_states[state->last_id].speedup;
  state->scs.uo = state->scs.u;
  state->scs.uoo = state->scs.u;
  state->scs.e = E_START;
  state->scs.eo = EO_START;

  // initialize variables used for calculating powerup
  state->pcs.u = state->control_states[state->last_id].cost;
  state->pcs.uo = state->pcs.u;
  state->pcs.uoo = state->pcs.u;
  state->pcs.e = E_START;
  state->pcs.eo = EO_START;

  state->low_state_iters = 0;
  state->idle_ns = 0;
  state->cost_estimate = R_ZERO;
  state->cost_xup_estimate = R_ZERO;

  // Calculate min and max speedup and powerup
  state->scs.umin = R_ONE;
  state->scs.umax = R_ONE;
  state->pcs.umin = R_ONE;
  state->pcs.umax = R_ONE;
  for (i = 0; i < state->num_system_states; i++) {
    real_t speedup = state->control_states[i].speedup;
    real_t cost = state->control_states[i].cost;
    if (speedup < state->scs.umin) {
      state->scs.umin = speedup < U_MIN_SPEEDUP ? U_MIN_SPEEDUP : speedup;
    }
    if (speedup >= state->scs.umax) {
      state->scs.umax = speedup;
    }
    if (cost <= state->pcs.umin) {
      state->pcs.umin = cost < U_MIN_COST ? U_MIN_COST : cost;
    }
    if (cost >= state->pcs.umax) {
      state->pcs.umax = cost;
    }
  }

  return state;
}

// Destroys poet state variable
void poet_destroy(poet_state * state) {
  if (state != NULL) {
    if (state->log_file != NULL) {
      fclose(state->log_file);
    }
    free(state->lb);
    free(state);
  }
}

// Change the constraint at runtime
void poet_set_constraint_type(poet_state * state,
                              poet_tradeoff_type_t constraint,
                              real_t goal) {
  if (state != NULL && goal > R_ZERO) {
    state->constraint = constraint;
    state->constraint_goal = goal;
  }
}

static inline void logger(const poet_state * state, unsigned long id,
                          real_t act_rate, real_t act_power,
                          real_t time_workload, real_t energy_workload) {
  unsigned int index;
  unsigned int i;

  if (state->log_file != NULL) {
    index = (id / state->period) % state->buffer_depth;
    // copy to log buffer
    state->lb[index].tag = id;
    state->lb[index].constraint = state->constraint;
    state->lb[index].act_rate = act_rate;
    state->lb[index].act_power = act_power;
    // performance data
    memcpy(&state->lb[index].pfs, &state->pfs, sizeof(filter_state));
    memcpy(&state->lb[index].scs, &state->scs, sizeof(calc_xup_state));
    // power data
    memcpy(&state->lb[index].cfs, &state->cfs, sizeof(filter_state));
    memcpy(&state->lb[index].pcs, &state->pcs, sizeof(calc_xup_state));
    // other data
    state->lb[index].time_workload = time_workload;
    state->lb[index].energy_workload = energy_workload;
    state->lb[index].lower_id = state->lower_id;
    state->lb[index].upper_id = state->upper_id;
    state->lb[index].low_state_iters = state->low_state_iters;
    state->lb[index].idle_ns = state->idle_ns;

    if (index == state->buffer_depth - 1) {
      for (i = 0; i < state->buffer_depth; i++) {
        const char* constraint;
        switch (state->constraint) {
          case POWER:
            constraint = "POWER";
            break;
          case PERFORMANCE:
          default:
            constraint = "PERFORMANCE";
        }
        fprintf(state->log_file, "%16lu %16s "
                "%16f %16f %16f %16f %16f %16f %16f %16f %16f "
                "%16f %16f %16f %16f %16f %16f %16f %16f %16f "
                "%16f %16f %16d %16d %16d %16llu\n",
                state->lb[i].tag,
                constraint,
                // performance data
                real_to_db(state->lb[i].act_rate),
                real_to_db(state->lb[i].pfs.x_hat_minus),
                real_to_db(state->lb[i].pfs.x_hat),
                real_to_db(state->lb[i].pfs.p_minus),
                real_to_db(state->lb[i].pfs.h),
                real_to_db(state->lb[i].pfs.k),
                real_to_db(state->lb[i].pfs.p),
                real_to_db(state->lb[i].scs.u),
                real_to_db(state->lb[i].scs.e),
                // power data
                real_to_db(state->lb[i].act_power),
                real_to_db(state->lb[i].cfs.x_hat_minus),
                real_to_db(state->lb[i].cfs.x_hat),
                real_to_db(state->lb[i].cfs.p_minus),
                real_to_db(state->lb[i].cfs.h),
                real_to_db(state->lb[i].cfs.k),
                real_to_db(state->lb[i].cfs.p),
                real_to_db(state->lb[i].pcs.u),
                real_to_db(state->lb[i].pcs.e),
                // other data
                real_to_db(state->lb[i].time_workload),
                real_to_db(state->lb[i].energy_workload),
                state->lb[i].lower_id,
                state->lb[i].upper_id,
                state->lb[i].low_state_iters,
                state->lb[i].idle_ns);
      }
    }
  }
}

/*
 * Estimates the base workload of the application by estimating
 * either the amount of time (in seconds) or the amount of energy
 * (in joules)  which elapses between iterations without any knobs
 * activated by poet.
 *
 * Uses a Kalman Filter
 */
static inline real_t estimate_base_workload(real_t current_workload,
                                            real_t last_xup,
                                            filter_state * state) {
  real_t _w;

  state->x_hat_minus = state->x_hat;
  state->p_minus = state->p + Q;

  state->h = last_xup;
  state->k = div(mult(state->p_minus, state->h),
                 mult3(state->h, state->p_minus, state->h) + R);
  state->x_hat = state->x_hat_minus + mult(state->k,
                  (current_workload - mult(state->h, state->x_hat_minus)));
  state->p = mult(R_ONE - mult(state->k, state->h), state->p_minus);

  _w = div(R_ONE, state->x_hat);

  return _w;
}

/*
 * Calculates the speedup or powerup necessary to achieve the target
 * performance or power rate.
 */
static inline void calculate_xup(real_t current_rate,
                                 real_t desired_rate,
                                 real_t w,
                                 calc_xup_state * state) {
  // A   = -(-P1*Z1 - P2*Z1 + MU*P1*P2 - MU*P2 + P2 - MU*P1 + P1 + MU)
  // B   = -(-MU*P1*P2*Z1 + P1*P2*Z1 + MU*P2*Z1 + MU*P1*Z1 - MU*Z1 - P1*P2)
  // C   = ((MU - MU*P1)*P2 + MU*P1 - MU)*w
  // D   = ((MU*P1-MU)*P2 - MU*P1 + MU)*w*Z1
  // F   = 1.0/(Z1-1.0)
  real_t A   = -(-mult(P1, Z1) - mult(P2, Z1) + mult3(MU, P1, P2) - mult(MU, P2) + P2 - mult(MU, P1) + P1 + MU);
  real_t B   = -(-mult4(MU, P1, P2, Z1) + mult3(P1, P2, Z1) + mult3(MU, P2, Z1) + mult3(MU, P1, Z1) - mult(MU, Z1) - mult(P1, P2));
  real_t C   = mult(mult(MU - mult(MU, P1), P2) + mult(MU, P1) - MU, w);
  real_t D   = mult3(mult(mult(MU, P1)-MU, P2) - mult(MU, P1) + MU, w, Z1);
  real_t F   = div(R_ONE, Z1 - R_ONE);

  state->e = desired_rate - current_rate;

  // Calculate speedup or powerup
  state->u = mult(F , mult(A, state->uo) + mult(B, state->uoo) + mult(C, state->e) + mult(D, state->eo));

  // Speedups/powerups less than the minimum have no effect
  if (state->u < state->umin) {
    state->u = state->umin;
  }

  // A speedup greater than the maximum is not achievable
  if (state->u > state->umax) {
    state->u = state->umax;
  }

  // Saving old state values
  state->uoo = state->uo;
  state->uo  = state->u;
  state->eo  = state->e;
}

/*
 * Configure the cost calc_xup_state.
 */
static inline void calculate_cost_xup(poet_state* state) {
  calc_xup_state* xup_state;
  switch (state->constraint) {
    case POWER:
      xup_state = &state->scs;
      break;
    case PERFORMANCE:
    default:
      xup_state = &state->pcs;
  }
  // cost xup values were previously computed
  xup_state->uoo = xup_state->uo;
  xup_state->u = state->cost_xup_estimate;
  xup_state->uo = xup_state->u;
  // reset error values
  xup_state->e = E_START;
  xup_state->eo = EO_START;
}

/*
 * Calculate the time division between the two system configuration states
 */
static inline void calculate_time_division(poet_state * state,
                                           real_t workload) {
  real_t cost;
  real_t cost_xup;
  real_t low_state_iters;
  real_t idle_ns;

  real_t lower_xup, partner_xup, upper_xup, target_xup;
  real_t lower_xup_cost, partner_xup_cost, upper_xup_cost;
  unsigned int partner_id = state->control_states[state->lower_id].idle_partner_id;
  switch (state->constraint) {
    case POWER:
      lower_xup = state->control_states[state->lower_id].cost;
      partner_xup = state->control_states[partner_id].cost;
      upper_xup = state->control_states[state->upper_id].cost;
      lower_xup_cost = state->control_states[state->lower_id].speedup;
      partner_xup_cost = state->control_states[partner_id].speedup;
      upper_xup_cost = state->control_states[state->upper_id].speedup;
      target_xup = state->pcs.u;
      break;
    case PERFORMANCE:
    default:
      lower_xup = state->control_states[state->lower_id].speedup;
      partner_xup = state->control_states[partner_id].speedup;
      upper_xup = state->control_states[state->upper_id].speedup;
      lower_xup_cost = state->control_states[state->lower_id].cost;
      partner_xup_cost = state->control_states[partner_id].cost;
      upper_xup_cost = state->control_states[state->upper_id].cost;
      target_xup = state->scs.u;
  }

  real_t r_period = int_to_real(state->period);
  if (lower_xup < R_ONE) {
    // this is an idle state

    // first determine required hybrid rate (combo of lower and partner rate)
    // period / target rate = 1 / (hybrid rate) + (period - 1) / (upper rate)
    // solve for hybrid rate
    real_t hybrid_xup = div(mult(target_xup, upper_xup),
                            mult(r_period, upper_xup - target_xup) + target_xup);

    if (hybrid_xup >= partner_xup) {
      // one iteration is already too long to be here, even without idling
      low_state_iters = 0;
      idle_ns = 0;
      cost = mult(div(r_period, upper_xup), upper_xup_cost);
      cost_xup = upper_xup_cost;
    } else {
      // compute percentage of first iteration to spend idling
      real_t x;
      real_t hybrid_xup_cost;
      if (lower_xup <= R_ZERO) {
        // hybrid rate = (1 - x) * (partner rate)
        x = R_ONE - div(hybrid_xup, partner_xup);
        hybrid_xup_cost = mult(x, lower_xup_cost) +
                          mult(R_ONE - x, partner_xup_cost);
      } else {
        // 1 / (hybrid rate) = x / (lower rate) + (1 - x) / (partner rate)
        x = div(mult(lower_xup, hybrid_xup - partner_xup),
                mult(hybrid_xup, lower_xup - partner_xup));
        hybrid_xup_cost = mult(div(x, lower_xup), lower_xup_cost) +
                          mult(div(R_ONE - x, partner_xup), partner_xup_cost);
      }

      real_t idle_sec = mult(workload,
                             div(R_ONE, hybrid_xup) - // time in first iteration
                             div(x, partner_xup));    // time in partner id
      idle_ns = real_to_int(mult(idle_sec, CONST(1000000000.0)));
      low_state_iters = 1;
      cost = mult(div(R_ONE, hybrid_xup), hybrid_xup_cost) +
             mult(div(r_period - R_ONE, upper_xup), upper_xup_cost);
      cost_xup = div(hybrid_xup_cost + mult(r_period - R_ONE, upper_xup_cost), r_period);
    }
  } else {
    // Calculate the time division between the upper and lower state
    // If lower rate and upper rate are equal, no need for time division
    real_t r_low_state_iters;
    if (upper_xup <= lower_xup && upper_xup >= lower_xup) {
      r_low_state_iters = R_ZERO;
    } else {
      // x represents the percentage of iterations spent in the first (lower)
      // configuration
      // Conversely, (1 - x) is the percentage of iterations in the second
      // (upper) configuration
      // This equation ensures the time period of the combined rates is equal
      // to the time period of the target rate
      // 1 / Target rate = X / (lower rate) + (1 - X) / (upper rate)
      // Solve for X
      real_t x = div(mult(upper_xup, lower_xup) - mult(target_xup, lower_xup),
                     mult(upper_xup, target_xup) - mult(target_xup, lower_xup));

      // Num of iterations (in lower state) = x * (controller period)
      r_low_state_iters = mult(r_period, x);
    }
    low_state_iters = real_to_int(r_low_state_iters);
    idle_ns = 0;
    r_low_state_iters = int_to_real(low_state_iters); // calculate actual cost
    cost = mult(div(r_low_state_iters, lower_xup), lower_xup_cost) +
           mult(div(r_period - r_low_state_iters, upper_xup), upper_xup_cost);
    cost_xup = div(mult(r_low_state_iters, lower_xup_cost) + mult(r_period - r_low_state_iters, upper_xup_cost), r_period);
  }

  state->low_state_iters = low_state_iters;
  state->idle_ns = idle_ns;
  state->cost_estimate = cost;
  state->cost_xup_estimate = cost_xup;
}

static inline real_t get_control_xup(poet_state * state, int id) {
  switch (state->constraint) {
    case POWER:
      return state->control_states[id].cost;
    case PERFORMANCE:
    default:
      return state->control_states[id].speedup;
  }
}

/**
 * Check all pairs of states that can achieve the target and choose the pair
 * with the lowest cost. Uses an n^2 algorithm.
 */
static inline void translate_n2_with_time(poet_state * state,
                                          real_t workload) {
  unsigned int i;
  unsigned int j;
  real_t target_xup;
  real_t best_cost;
  real_t best_cost_xup = -1;
  int best_lower_id = -1;
  int best_upper_id = -1;
  int best_low_state_iters = -1;
  unsigned long long best_idle_ns = 0;
  real_t lower_xup;
  real_t upper_xup;
  int is_best;
  int disable_idle = getenv(POET_DISABLE_IDLE) == NULL ? 0 : 1;

  switch (state->constraint) {
    case POWER:
      target_xup = state->pcs.u;
      best_cost = R_ZERO;
      break;
    case PERFORMANCE:
    default:
      target_xup = state->scs.u;
      best_cost = BIG_REAL_T;
  }

  for (i = 0; i < state->num_system_states; i++) {
    upper_xup = get_control_xup(state, i);
    if (upper_xup < target_xup || upper_xup < R_ONE) {
      // upper_id cannot be an idle state
      continue;
    }
    state->upper_id = i;
    for (j = 0; j < state->num_system_states; j++) {
      lower_xup = get_control_xup(state, j);
      if (lower_xup > target_xup ||
          (lower_xup < R_ONE && disable_idle > 0)) {
        continue;
      }
      state->lower_id = j;
      // find time for both states
      calculate_time_division(state, workload);
      // if this is the best configuration so far, remember it
      switch (state->constraint) {
        case POWER:
          // maximize performance
          is_best = state->cost_estimate > best_cost ? 1 : 0;
          break;
        case PERFORMANCE:
        default:
          // minimize power
          is_best = state->cost_estimate < best_cost ? 1 : 0;
      }
      if (is_best > 0) {
        best_lower_id = j;
        best_upper_id = i;
        best_low_state_iters = state->low_state_iters;
        best_idle_ns = state->idle_ns;
        best_cost = state->cost_estimate;
        best_cost_xup = state->cost_xup_estimate;
      }
      state->idle_ns = 0;
    }
  }

  // use the best configuration
  state->lower_id = best_lower_id;
  state->upper_id = best_upper_id;
  state->low_state_iters = best_low_state_iters;
  state->idle_ns = best_idle_ns;
  state->cost_estimate = best_cost;
  state->cost_xup_estimate = best_cost_xup;
}

// Runs POET decision engine and requests system changes
void poet_apply_control(poet_state * state,
                        unsigned long id,
                        real_t perf,
                        real_t pwr) {
  if (state == NULL || getenv(POET_DISABLE_CONTROL) != NULL) {
    return;
  }

  if (state->current_action == 0) {
    // Estimate the performance workload
    // estimate time between iterations given minimum amount of resources
    real_t time_workload = estimate_base_workload(perf,
                                                  state->scs.u,
                                                  &state->pfs);
    // Estimate the cost workload
    // estimate energy between iterations given minimum amount of resources
    real_t energy_workload = estimate_base_workload(pwr,
                                                    state->pcs.u,
                                                    &state->cfs);

    // Get a new goal speedup or powerup to apply to the application
    real_t workload;
    switch (state->constraint) {
      case POWER:
        calculate_xup(pwr, state->constraint_goal, energy_workload, &state->pcs);
        workload = energy_workload;
        break;
      case PERFORMANCE:
      default:
        calculate_xup(perf, state->constraint_goal, time_workload, &state->scs);
        workload = time_workload;
    }

    // Xup is translated into a system configuration
    // A certain amount of time is assigned to each system configuration
    // in order to achieve the requested Xup
    translate_n2_with_time(state, workload);
    calculate_cost_xup(state);

    logger(state, id,
           perf, pwr,
           time_workload, energy_workload);
  }

  // Check which speedup should be applied, upper or lower
  int config_id = -1;
  if (state->low_state_iters > 0) {
    config_id = state->lower_id;
    state->low_state_iters--;
  } else if (state->upper_id >= 0) {
    config_id = state->upper_id;
  }

  if (config_id >= 0 && ((unsigned int) config_id != state->last_id || state->is_first_apply > 0)) {
    if (state->apply != NULL && getenv(POET_DISABLE_APPLY) == NULL) {
      state->apply(state->apply_states, state->num_system_states, config_id,
                   state->last_id, state->idle_ns, state->is_first_apply);
      state->is_first_apply = 0;
    }
    state->last_id = config_id;
    // only allow idle once per period
    state->idle_ns = 0;
  }

  state->current_action = (state->current_action + 1) % state->period;
}
