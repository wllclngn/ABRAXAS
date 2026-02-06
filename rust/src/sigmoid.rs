//! Sigmoid transition math.
//!
//! Dusk is canonical: day -> night over DUSK_DURATION centered on sunset.
//! Dawn is its inverse: night -> day over DAWN_DURATION centered on sunrise.
//! Manual overrides use the same sigmoid over [0, duration].

use crate::{
    DAWN_DURATION, DUSK_DURATION, DUSK_OFFSET, SIGMOID_STEEPNESS, TEMP_DAY_CLEAR, TEMP_DAY_DARK,
    TEMP_NIGHT,
};
use crate::solar;

const SECONDS_PER_DAY: i64 = 86400;

fn sigmoid_raw(x: f64, steepness: f64) -> f64 {
    1.0 / (1.0 + (-steepness * x).exp())
}

pub fn sigmoid_norm(x: f64, steepness: f64) -> f64 {
    let raw = sigmoid_raw(x, steepness);
    let low = sigmoid_raw(-1.0, steepness);
    let high = sigmoid_raw(1.0, steepness);
    (raw - low) / (high - low)
}

pub fn calculate_solar_temp(
    minutes_from_sunrise: f64,
    minutes_to_sunset: f64,
    is_dark_mode: bool,
) -> i32 {
    let day_temp = if is_dark_mode {
        TEMP_DAY_DARK
    } else {
        TEMP_DAY_CLEAR
    };
    let night_temp = TEMP_NIGHT;

    let dawn_half = DAWN_DURATION / 2.0;
    let dusk_half = DUSK_DURATION / 2.0;

    // Dawn: night -> day (inverse of dusk)
    if minutes_from_sunrise.abs() < dawn_half {
        let x = minutes_from_sunrise / dawn_half; // [-1, 1]
        let factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
        return (night_temp as f64 + (day_temp - night_temp) as f64 * factor) as i32;
    }

    // Dusk: day -> night (canonical, midpoint offset before sunset)
    let dusk_shifted = minutes_to_sunset - DUSK_OFFSET;
    if dusk_shifted.abs() < dusk_half {
        let x = dusk_shifted / dusk_half; // [1, -1]
        let factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
        return (night_temp as f64 + (day_temp - night_temp) as f64 * factor) as i32;
    }

    // Daytime (between windows)
    if minutes_from_sunrise >= dawn_half && dusk_shifted >= dusk_half {
        return day_temp;
    }

    // Night
    night_temp
}

pub fn calculate_manual_temp(
    start_temp: i32,
    target_temp: i32,
    start_time: i64,
    duration_min: i32,
    now: i64,
) -> i32 {
    if duration_min <= 0 {
        return target_temp;
    }

    let elapsed_min = (now - start_time) as f64 / 60.0;

    if elapsed_min >= duration_min as f64 {
        return target_temp;
    }

    // Map [0, duration] -> [-1, 1]
    let x = 2.0 * (elapsed_min / duration_min as f64) - 1.0;
    let factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
    (start_temp as f64 + (target_temp - start_temp) as f64 * factor) as i32
}

/// Calculate next time to auto-resume solar control after a manual override.
/// Returns the epoch time 15 minutes before the next dawn/dusk transition window.
pub fn next_transition_resume(now: i64, lat: f64, lon: f64) -> i64 {
    let st = match solar::sunrise_sunset(now, lat, lon) {
        Some(st) => st,
        None => return now + SECONDS_PER_DAY, // polar fallback: 24h
    };

    let dawn_window_start = st.sunrise - (DAWN_DURATION / 2.0 * 60.0) as i64;
    let dusk_window_start = st.sunset - ((DUSK_DURATION / 2.0 + DUSK_OFFSET) * 60.0) as i64;

    let resume_dawn = dawn_window_start - 15 * 60;
    let resume_dusk = dusk_window_start - 15 * 60;

    // Find earliest future candidate
    let mut best: i64 = 0;
    if resume_dawn > now {
        best = resume_dawn;
    }
    if resume_dusk > now && (best == 0 || resume_dusk < best) {
        best = resume_dusk;
    }

    if best > 0 {
        return best;
    }

    // Both today's transitions passed -- use tomorrow's dawn
    let tomorrow = now + SECONDS_PER_DAY;
    match solar::sunrise_sunset(tomorrow, lat, lon) {
        Some(st2) => st2.sunrise - ((DAWN_DURATION / 2.0 + 15.0) * 60.0) as i64,
        None => now + SECONDS_PER_DAY,
    }
}

