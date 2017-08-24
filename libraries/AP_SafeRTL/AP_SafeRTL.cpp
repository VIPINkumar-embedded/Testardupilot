/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "AP_SafeRTL.h"

extern const AP_HAL::HAL& hal;

const AP_Param::GroupInfo AP_SafeRTL::var_info[] = {
    // @Param: ACCURACY
    // @DisplayName: SafeRTL _accuracy
    // @Description: SafeRTL _accuracy. The minimum distance between points.
    // @Units: m
    // @Range: 0 10
    // @User: Advanced
    AP_GROUPINFO("ACCURACY", 0, AP_SafeRTL, _accuracy, SAFERTL_ACCURACY_DEFAULT),

    // @Param: POINTS
    // @DisplayName: SafeRTL maximum number of points on path
    // @Description: SafeRTL maximum number of points on path. Set to 0 to disable SafeRTL.  100 points consumes about 3k of memory.
    // @Range: 0 500
    // @User: Advanced
    // @RebootRequired: True
    AP_GROUPINFO("POINTS", 1, AP_SafeRTL, _points_max, SAFERTL_POINTS_DEFAULT),

    AP_GROUPEND
};

/*
*    This library is used for the Safe Return-to-Launch feature. The vehicle's position
*    (aka "bread crumbs") are stored into an array in memory at regular intervals.
*    After a certain number of bread crumbs have been stored and space within the array
*    is low, clean-up algorithms are run to reduce the total number of points.
*    When Safe-RTL is initiated by the vehicle code, a more thorough cleanup runs and
*    the resulting path is fed into navigation controller to return the vehicle to home.
*
*    The cleanup consists of two parts, pruning and simplification:
*
*    1. Pruning calculates the closest distance between two line segments formed by
*    two pairs of sequential points, and then cuts out anything between two points
*    when their line segments get close. This algorithm will never compare two
*    consecutive line segments. Obviously the segments (p1,p2) and (p2,p3) will
*    get very close (they touch), but there would be nothing to trim between them.
*
*    2. Simplification uses the Ramer-Douglas-Peucker algorithm. See Wikipedia for
*    a more complete description.
*
*    The simplification and pruning algorithms run in the background and do not alter
*    the path in memory.  Two definitions, SAFERTL_SIMPLIFY_TIME_US and
*    SAFERTL_LOOP_TIME_US are used to limit how long each algorithm will be run
*    before they save their state and return.
*
*    Both algorithm are "anytime algorithms" meaning they can be interrupted before
*    they complete which is helpful when memory is filling up and we just need to
*    quickly identify a handful of points which can be deleted.
*
*    Once the algorithms have completed the _simplify_complete and _prune_complete
*    flags are set to true.  The "thorough cleanup" procedure which is run as the
*    vehicle initiates RTL, waits for these flags to become true.  This can force
*    the vehicle to pause for a few seconds before initiating the return journey.
*/

AP_SafeRTL::AP_SafeRTL(const AP_AHRS& ahrs) :
    _ahrs(ahrs)
{
    AP_Param::setup_object_defaults(this, var_info);
    _simplify_bitmask.setall();
}

// initialise safe rtl including setting up background processes
void AP_SafeRTL::init()
{
    // protect against repeated call to init
    if (_path != nullptr) {
        return;
    }

    // constrain the path length, in case the user decided to make the path unreasonably long.
    _points_max = constrain_int16(_points_max, 0, SAFERTL_POINTS_MAX);

    // check if user has disabled SafeRTL
    if (_points_max == 0 || !is_positive(_accuracy)) {
        return;
    }

    // allocate arrays
    _path = (Vector3f*)calloc(_points_max, sizeof(Vector3f));

    _prunable_loops_max = _points_max * SAFERTL_PRUNING_LOOP_BUFFER_LEN_MULT;
    _prunable_loops = (prune_loop_t*)calloc(_prunable_loops_max, sizeof(prune_loop_t));

    _simplify_stack_max = _points_max * SAFERTL_SIMPLIFY_STACK_LEN_MULT;
    _simplify_stack = (simplify_start_finish_t*)calloc(_simplify_stack_max, sizeof(simplify_start_finish_t));

    // check if memory allocation failed
    if (_path == nullptr || _prunable_loops == nullptr || _simplify_stack == nullptr) {
        log_action(SRTL_DEACTIVATED_INIT_FAILED);
        gcs().send_text(MAV_SEVERITY_WARNING, "SafeRTL deactivated: init failed");
        free(_path);
        free(_prunable_loops);
        free(_simplify_stack);
        return;
    }

    _path_points_max = _points_max;

    // register SafeRTL cleanup methods to run in IO thread
    hal.scheduler->register_io_process(FUNCTOR_BIND_MEMBER(&AP_SafeRTL::detect_simplifications, void));
    hal.scheduler->register_io_process(FUNCTOR_BIND_MEMBER(&AP_SafeRTL::detect_loops, void));
}

// clear return path and set home location.  This should be called as part of the arming procedure
void AP_SafeRTL::reset_path(bool position_ok)
{
    Vector3f current_pos;
    position_ok &= _ahrs.get_relative_position_NED_origin(current_pos);
    reset_path(position_ok, current_pos);
}

void AP_SafeRTL::reset_path(bool position_ok, const Vector3f& current_pos)
{
    if (_path == nullptr) {
        return;
    }

    // reset simplification
    reset_simplification();
    // reset pruning
    reset_pruning();
    // clear path
    _path_last_index = 0;

    // de-activate if no position at take-off
    if (!position_ok) {
        _active = false;
        log_action(SRTL_DEACTIVATED_BAD_POSITION);
        gcs().send_text(MAV_SEVERITY_WARNING, "SafeRTL deactivated: bad position");
        return;
    }

    // save current position as first point in path
    _path[_path_last_index] = current_pos;
    _last_good_position_ms = AP_HAL::millis();
    _active = true;
}

// call this a couple of times per second regardless of what mode the vehicle is in
void AP_SafeRTL::update(bool position_ok, bool save_position)
{
    if (!_active || !position_ok || !save_position) {
        return;
    }

    Vector3f current_pos;
    position_ok &= _ahrs.get_relative_position_NED_origin(current_pos);
    update(position_ok, current_pos);
}

void AP_SafeRTL::update(bool position_ok, const Vector3f& current_pos)
{
    if (!_active) {
        return;
    }

    if (position_ok) {
        _last_good_position_ms = AP_HAL::millis();
    } else {
        if (AP_HAL::millis() - _last_good_position_ms > SAFERTL_BAD_POSITION_TIMEOUT) {
            _active = false;
            log_action(SRTL_DEACTIVATED_BAD_POSITION);
            gcs().send_text(MAV_SEVERITY_WARNING,"SafeRTL deactivated: bad position");
        }
        return;
    }

    // it's important to do the cleanup before adding the point, because appending a point will reset the cleanup methods,
    // so there will not be anything to clean up immediately after adding a point.
    // The cleanup usually returns immediately. If it decides to actually perform the cleanup, it takes about 100us.
    if (!routine_cleanup()) {
        _active = false;
        log_action(SRTL_DEACTIVATED_CLEANUP_FAILED);
        gcs().send_text(MAV_SEVERITY_WARNING,"SafeRTL deactivated: path cleanup failed");
        return;
    }

    if (position_ok) {
        // append the new point, if we have traveled far enough
        if (HYPOT(current_pos, _path[_path_last_index]) > _accuracy) {
            // add the breadcrumb
            _path[++_path_last_index] = current_pos;

            log_action(SRTL_POINT_ADD, current_pos);

            // if cleanup algorithms are finished (and therefore not running), reset them
            if (_simplify_complete) {
                restart_simplification();
            }
            if (_prune_complete) {
                restart_pruning();
            }
        }
    }
}

// perform thorough cleanup including simplification, pruning and removal of all unnecessary points
// returns true if the thorough cleanup was completed, false if it has not yet completed
// this method should be called repeatedly until it returns true before initiating the return journey
bool AP_SafeRTL::thorough_cleanup()
{
    // this should never happen but just in case
    if (!_active) {
        return false;
    }

    // check if we are ready to perform cleanup.  this should be called just before thorough_cleanup just before initiating the RTL
    if (!_prune_complete || !_simplify_complete) {
        return false;
    }

    // apply simplification
    zero_points_by_simplify_bitmask();

    // apply pruning, prune every single loop
    zero_points_by_loops(SAFERTL_POINTS_MAX);

    remove_empty_points();

    // end by resetting the state of the cleanup methods.
    reset_simplification();
    reset_pruning();

    return true;
}

// get next point on the path to home, returns true on success
bool AP_SafeRTL::pop_point(Vector3f& point)
{
    // check we have a point
    if (!_active || _path_last_index < 0) {
        return false;
    }

    // return last point and remove from path
    point = _path[_path_last_index--];
    return true;
}

// returns number of points on the path
uint16_t AP_SafeRTL::get_num_points()
{
    return _path_last_index+1;
}

// Simplifies a 3D path, according to the Ramer-Douglas-Peucker algorithm.
// _simplify_complete is set to true when all simplifications on the path have been identified
void AP_SafeRTL::detect_simplifications()
{
    if (!_active || _simplify_complete || _path_last_index < 2) {
        return;
    }

    // if not complete but also nothing to do, we must be restarting
    if (_simplify_stack_last_index == -1) {
        // reset to beginning state
        _simplify_stack[++_simplify_stack_last_index] = simplify_start_finish_t {0, _path_last_index};
    }

    const uint32_t start_time_us = AP_HAL::micros();
    while (_simplify_stack_last_index >= 0) { // while there is something to do
        if (AP_HAL::micros() - start_time_us > SAFERTL_SIMPLIFY_TIME_US) {
            return;
        }

        const simplify_start_finish_t tmp = _simplify_stack[_simplify_stack_last_index--];
        const int16_t start_index = tmp.start;
        const int16_t end_index = tmp.finish;

        // if we've already verified that everything before here is clean
        if (end_index <= _simplify_clean_until) {
            continue;
        }

        float max_dist = 0.0f;
        int16_t index = start_index;
        for (int16_t i = index + 1; i < end_index; i++) {
            if (_simplify_bitmask.get(i)) {
                const float dist = point_line_dist(_path[i], _path[start_index], _path[end_index]);
                if (dist > max_dist) {
                    index = i;
                    max_dist = dist;
                }
            }
        }

        if (max_dist > SAFERTL_SIMPLIFY_EPSILON) {
            // if the to-do list is full, give up on simplifying. This should never happen.
            if (_simplify_stack_last_index > _simplify_stack_max) {
                _simplify_complete = true;
                return;
            }
            _simplify_stack[++_simplify_stack_last_index] = simplify_start_finish_t {start_index, index};
            _simplify_stack[++_simplify_stack_last_index] = simplify_start_finish_t {index, end_index};
        } else {
            for (int16_t i = start_index + 1; i < end_index; i++) {
                _simplify_bitmask.clear(i);
            }
        }
    }
    _simplify_complete = true;
}

/**
*   This method runs for the allotted time, and detects loops in a path. Any detected loops are added to _prunable_loops,
*   this function does not alter the path in memory. It works by comparing the line segment between any two sequential points
*   to the line segment between any other two sequential points. If they get close enough, anything between them could be pruned.
*
*   Note that this method might take a bit longer than SAFERTL_LOOP_TIME_US. It only stops after it's already run longer.
*/
void AP_SafeRTL::detect_loops()
{
    // if SafeRTL is not active OR if this algorithm has already run to completion OR there's fewer than 3 points in the path
    if (!_active || _prune_complete || _path_last_index < 3) {
        return;
    }
    const uint32_t start_time_us = AP_HAL::micros();

    while (_prune_current_i < _path_last_index - 1) {
        // if this method has run for long enough, exit
        if (AP_HAL::micros() - start_time_us > SAFERTL_LOOP_TIME_US) {
            return;
        }

        // this check prevents detection of a loop-within-a-loop
        int16_t j = MAX(_prune_current_i + 2, _prune_min_j);
        while (j < _path_last_index) {
            dist_point dp = segment_segment_dist(_path[_prune_current_i], _path[_prune_current_i+1], _path[j], _path[j+1]);
            if (dp.distance <= SAFERTL_PRUNING_DELTA) { // if there is a loop here
                _prune_min_j = j;
                // if the buffer is full
                if ( _prunable_loops_last_index >= _prunable_loops_max - 1) {
                    _prune_complete = true; // pruning is effectively complete now, since there's no reason to continue looking for them.
                    return;
                }
                // int promotion rules disallow using i+1 and j+1
                _prunable_loops[++_prunable_loops_last_index] = {(++_prune_current_i)--,(++j)--,dp.midpoint};
            }
            j++;
        }
        _prune_current_i++;
    }
    _prune_complete = true;
}

//
// Private methods
//

/**
*   Run this regularly, in the main loop (don't worry - it runs quickly, 100us). If no cleanup is needed, it will immediately return.
*   Otherwise, it will run a cleanup, based on info computed by the background methods, detect_simplifications() and detect_loops().
*   If no cleanup is possible, this method returns false. This should be treated as an error condition.
*/
bool AP_SafeRTL::routine_cleanup()
{
    // We only do a routine cleanup if the memory is almost full. Cleanup deletes
    // points which are potentially useful, so it would be bad to clean up if we don't have to
    if (_path_last_index < MAX(_path_points_max - SAFERTL_CLEANUP_START_MARGIN, 0)) {
        return true;
    }

    int16_t potential_amount_to_simplify = _simplify_bitmask.size() - _simplify_bitmask.count();

    // if simplifying will remove more than 10 points, just do it
    if (potential_amount_to_simplify >= SAFERTL_CLEANUP_POINT_MIN) {
        zero_points_by_simplify_bitmask();
        remove_empty_points();
        // end by resetting the state of the cleanup methods.
        reset_simplification();
        reset_pruning();
        return true;
    }

    int16_t potential_amount_to_prune = 0;
    for (int16_t i = 0; i <= _prunable_loops_last_index; i++) {
        // add 1 at the end, because a pruned loop is always replaced by one new point.
        potential_amount_to_prune += _prunable_loops[i].end_index - _prunable_loops[i].start_index + 1;
    }

    // if pruning could remove 10+ points, prune loops until 10 or more points have been removed (doesn't necessarily prune all loops)
    if (potential_amount_to_prune >= SAFERTL_CLEANUP_POINT_MIN) {
        zero_points_by_loops(SAFERTL_CLEANUP_POINT_MIN);
        remove_empty_points();
        // end by resetting the state of the cleanup methods.
        reset_simplification();
        reset_pruning();
        return true;
    }

    // as a last resort, see if pruning and simplifying together would remove 10+ points.
    if (potential_amount_to_prune + potential_amount_to_simplify >= SAFERTL_CLEANUP_POINT_MIN) {
        zero_points_by_simplify_bitmask();
        zero_points_by_loops(SAFERTL_CLEANUP_POINT_MIN);
        remove_empty_points();
        // end by resetting the state of the cleanup methods.
        reset_simplification();
        reset_pruning();
        return true;
    }
    return false;
}

// restart simplification algorithm, should be called whenever a new point is added
void AP_SafeRTL::restart_simplification()
{
    _simplify_complete = false;
    _simplify_stack_last_index = -1;
    _simplify_bitmask.setall();
}

// reset simplification algorithm so that it will re-check all points in the path
// should be called if the existing path is altered for example when a loop as been removed
void AP_SafeRTL::reset_simplification()
{
    _simplify_clean_until = 0;
    restart_simplification();
}

// restart pruning algorithm, should be called whenever a new point is added
void AP_SafeRTL::restart_pruning()
{
    _prune_complete = false;
    _prune_current_i = _prune_clean_until;
    _prune_min_j = _prune_clean_until+2;
    _prunable_loops_last_index = -1; // clear the loops that we've recorded
}

// reset pruning algorithm so that it will re-check all points in the path
// should be called if the existing path is altered for example when a loop as been removed
void AP_SafeRTL::reset_pruning()
{
    _prune_clean_until = 0;
    restart_pruning();
}

void AP_SafeRTL::zero_points_by_simplify_bitmask()
{
    for (int16_t i = 0; i <= _path_last_index; i++) {
        if (!_simplify_bitmask.get(i)) {
            _simplify_clean_until = MIN(_simplify_clean_until, i-1);
            if (!_path[i].is_zero()) {
                log_action(SRTL_POINT_SIMPLIFY, _path[i]);
                _path[i].zero();
            }
        }
    }
}

// prunes loops until points_to_delete points have been removed. It does not necessarily prune all loops.
void AP_SafeRTL::zero_points_by_loops(uint16_t points_to_delete)
{
    uint16_t removed_points = 0;
    for (int16_t i = 0; i <= _prunable_loops_last_index; i++) {
        prune_loop_t l = _prunable_loops[i];
        _prune_clean_until = MIN(_prune_clean_until, l.start_index-1);
        for (int16_t j = l.start_index; j < l.end_index; j++) {
            // zero this point if it wasn't already zeroed
            if (!_path[j].is_zero()) {
                log_action(SRTL_POINT_PRUNE, _path[j]);
                _path[j].zero();
            }
        }
        _path[(int16_t)((l.start_index+l.end_index)/2.0)] = l.midpoint;
        removed_points += l.end_index - l.start_index - 1;
        if (removed_points > points_to_delete) {
            return;
        }
    }
}

/**
*  Removes all 0,0,0 points from the path, and shifts remaining items to correct position.
*  The first item will not be removed.
*/
void AP_SafeRTL::remove_empty_points()
{
    int16_t src = 0;
    int16_t dest = 0;
    uint16_t removed = 0;
    while (++src <= _path_last_index) { // never removes the first point
        if (!_path[src].is_zero()) {
            _path[++dest] = _path[src];
        } else {
            removed++;
        }
    }
    _path_last_index -= removed;
}

/**
*  Returns the closest distance in 3D space between any part of two input segments, defined from p1 to p2 and from p3 to p4.
*  Also returns the point which is halfway between
*
*  Limitation: This function does not work for parallel lines. In this case, it will return FLT_MAX. This does not matter for the path cleanup algorithm because
*  the pruning will still occur fine between the first parallel segment and a segment which is directly before or after the second segment.
*/
AP_SafeRTL::dist_point AP_SafeRTL::segment_segment_dist(const Vector3f &p1, const Vector3f &p2, const Vector3f &p3, const Vector3f &p4)
{
    Vector3f line1 = p2-p1;
    Vector3f line2 = p4-p3;
    Vector3f line_start_diff = p1-p3; // from the beginning of the second line to the beginning of the first line

    // these don't really have a physical representation. They're only here to break up the longer formulas below.
    float a = line1*line1;
    float b = line1*line2;
    float c = line2*line2;
    float d = line1*line_start_diff;
    float e = line2*line_start_diff;

    // the parameter for the position on line1 and line2 which define the closest points.
    float t1 = 0.0f;
    float t2 = 0.0f;

    // if lines are almost parallel, return a garbage answer. This is irrelevant, since the loop
    // could always be pruned start/end of the previous/subsequent line segment
    if (is_zero((a*c)-(b*b))) {
        return {FLT_MAX, Vector3f(0.0f, 0.0f, 0.0f)};
    } else {
        t1 = (b*e-c*d)/(a*c-b*b);
        t2 = (a*e-b*d)/(a*c-b*b);

        // restrict both parameters between 0 and 1.
        t1 = constrain_float(t1, 0.0f, 1.0f);
        t2 = constrain_float(t2, 0.0f, 1.0f);

        // difference between two closest points
        Vector3f dP = line_start_diff+line1*t1-line2*t2;

        Vector3f midpoint = (p1+line1*t1 + p3+line2*t2)/2.0f;
        return {dP.length(), midpoint};
    }
}


/**
*  Returns the closest distance from a point to a 3D line. The line is defined by any 2 points
*/
float AP_SafeRTL::point_line_dist(const Vector3f &point, const Vector3f &line1, const Vector3f &line2)
{
    // triangle side lengths
    float a = HYPOT(point, line1);
    float b = HYPOT(line1, line2);
    float c = HYPOT(line2, point);

    // protect against divide by zero later
    if (is_zero(b)) {
        return 0.0f;
    }

    // semiperimeter of triangle
    float s = (a+b+c)/2.0f;

    float area_squared = s*(s-a)*(s-b)*(s-c);
    // must be constrained above 0 because a triangle where all 3 points could be on a line. float rounding could push this under 0.
    if (area_squared < 0.0f) {
        area_squared = 0.0f;
    }
    float area = safe_sqrt(area_squared);
    return 2.0f*area/b;
}

// logging
void AP_SafeRTL::log_action(SRTL_Actions action, const Vector3f point)
{
    DataFlash_Class::instance()->Log_Write_SRTL(_active, _path_last_index, _path_points_max, action, point);
}
