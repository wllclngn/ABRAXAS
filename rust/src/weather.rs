//! NOAA weather API client.
//!
//! Two-step API:
//!   1. GET https://api.weather.gov/points/{lat},{lon}
//!      -> extract properties.forecastHourly URL
//!   2. GET that URL
//!      -> extract first period's shortForecast, temperature, isDaytime
//!
//! When compiled without the "noaa" feature, all functions are no-ops.

use crate::config::WeatherData;
use crate::now_epoch;

#[cfg(feature = "noaa")]
pub fn init() {}

#[cfg(feature = "noaa")]
pub fn cleanup() {}

#[cfg(feature = "noaa")]
pub fn fetch(lat: f64, lon: f64) -> WeatherData {
    match fetch_inner(lat, lon) {
        Ok(wd) => wd,
        Err(_) => WeatherData {
            cloud_cover: 0,
            forecast: "Unknown".to_string(),
            temperature: 0.0,
            is_day: true,
            fetched_at: now_epoch(),
            has_error: true,
        },
    }
}

#[cfg(feature = "noaa")]
fn get_agent() -> &'static ureq::Agent {
    use std::sync::OnceLock;
    static AGENT: OnceLock<ureq::Agent> = OnceLock::new();
    AGENT.get_or_init(|| {
        ureq::AgentBuilder::new()
            .timeout_read(std::time::Duration::from_secs(5))
            .timeout_write(std::time::Duration::from_secs(5))
            .user_agent("abraxas/4.0 (weather color temp daemon)")
            .build()
    })
}

#[cfg(feature = "noaa")]
fn fetch_inner(lat: f64, lon: f64) -> Result<WeatherData, Box<dyn std::error::Error>> {
    let agent = get_agent();

    // Step 1: Get grid point
    let url = format!("https://api.weather.gov/points/{:.4},{:.4}", lat, lon);
    let resp: serde_json::Value = agent
        .get(&url)
        .set("Accept", "application/geo+json")
        .call()?
        .into_json()?;

    let forecast_url = resp["properties"]["forecastHourly"]
        .as_str()
        .ok_or("No forecastHourly URL")?
        .to_string();

    // Step 2: Get hourly forecast
    let resp: serde_json::Value = agent
        .get(&forecast_url)
        .set("Accept", "application/geo+json")
        .call()?
        .into_json()?;

    let period = &resp["properties"]["periods"][0];
    if period.is_null() {
        return Err("No forecast periods".into());
    }

    let short_forecast = period["shortForecast"]
        .as_str()
        .unwrap_or("Unknown")
        .to_string();
    let temperature = period["temperature"].as_f64().unwrap_or(0.0);
    let is_day = period["isDaytime"].as_bool().unwrap_or(true);

    let cloud_cover = cloud_cover_from_forecast(&short_forecast);

    Ok(WeatherData {
        cloud_cover,
        forecast: short_forecast,
        temperature,
        is_day,
        fetched_at: now_epoch(),
        has_error: false,
    })
}

#[cfg(feature = "noaa")]
fn cloud_cover_from_forecast(forecast: &str) -> i32 {
    let lower = forecast.to_lowercase();

    // Precipitation always means heavy cloud
    if lower.contains("rain")
        || lower.contains("storm")
        || lower.contains("snow")
        || lower.contains("drizzle")
        || lower.contains("showers")
    {
        return 95;
    }

    if lower.contains("overcast") {
        return 90;
    }

    // Mostly cloudy (before general "cloudy" check)
    if lower.contains("mostly cloudy") {
        return 75;
    }

    if lower.contains("cloudy") {
        return 90;
    }

    if lower.contains("partly") {
        return 50;
    }

    // Mostly sunny/clear (before general "sunny"/"clear")
    if lower.contains("mostly sunny") || lower.contains("mostly clear") {
        return 25;
    }

    if lower.contains("sunny") || lower.contains("clear") {
        return 10;
    }

    0
}

// Non-NOAA stubs
#[cfg(not(feature = "noaa"))]
pub fn init() {}

#[cfg(not(feature = "noaa"))]
pub fn cleanup() {}

#[cfg(not(feature = "noaa"))]
pub fn fetch(_lat: f64, _lon: f64) -> WeatherData {
    WeatherData {
        cloud_cover: 0,
        forecast: "Disabled (non-USA build)".to_string(),
        temperature: 0.0,
        is_day: true,
        fetched_at: now_epoch(),
        has_error: true,
    }
}
