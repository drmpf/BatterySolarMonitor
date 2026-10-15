# Solar Shading Model

**Method**: Back-solve per-angle shade factors from 1204 days of Enphase daily production vs Open-Meteo hourly GTI predictions

**Resolution**: 10 deg azimuth x 5 deg elevation bins
**Sun position bins**: 214 total, 203 with significant energy

## Model Accuracy

| Model | RMSE (kWh) | MAE (kWh) | Bias (kWh) | R^2 |
|-------|----------:|---------:|-----------:|----:|
| Old (uniform 0.80) | 6.25 | 4.92 | +2.37 | 0.793 |
| **Shading model** | **5.99** | **4.44** | **-1.08** | **0.810** |
| Improvement | 4.1% | 9.9% | 1.08 vs 2.37 | -- |

Bias is mean(actual - predicted): positive = model under-predicts, negative = over-predicts.

## Seasonal Accuracy (Shading Model)

| Season | Days | RMSE (kWh) | MAE (kWh) | Bias (kWh) |
|--------|-----:|----------:|---------:|-----------:|
| Summer (Nov-Feb) | 361 | 7.77 | 5.91 | -2.56 |
| Equinox (Mar-Apr/Sep-Oct) | 391 | 5.51 | 4.22 | -0.53 |
| Winter (May-Aug) | 452 | 4.59 | 3.45 | -0.37 |

## Shade Factors by Direction

| Direction | Avg Shade Factor | Energy Contribution |
|-----------|:----------------:|:-------------------:|
| North (350-10 deg) | 0.929 | 17.0% |
| NE (20-70 deg) | 1.008 | 35.6% |
| East (80-100 deg) | 0.805 | 9.0% |
| SW (200-250 deg) | 0.587 | 0.8% |
| West (260-280 deg) | 0.653 | 8.8% |
| NW (290-340 deg) | 0.653 | 28.7% |

## Shading Hot-Spots (shade factor < 0.70)

| Azimuth | Elevation | Shade Factor | Energy (kWh) |
|--------:|----------:|:------------:|:------------:|
| 340-350 deg | 30-35 deg | 0.440 | 1188 |
| 320-330 deg | 25-30 deg | 0.310 | 995 |
| 270-280 deg | 45-50 deg | 0.648 | 833 |
| 310-320 deg | 40-45 deg | 0.550 | 634 |
| 320-330 deg | 30-35 deg | 0.240 | 600 |
| 300-310 deg | 25-30 deg | 0.130 | 435 |
| 310-320 deg | 15-20 deg | 0.190 | 409 |
| 310-320 deg | 20-25 deg | 0.240 | 409 |
| 340-350 deg | 35-40 deg | 0.610 | 408 |
| 260-270 deg | 35-40 deg | 0.641 | 361 |
| 310-320 deg | 35-40 deg | 0.397 | 353 |
| 280-290 deg | 45-50 deg | 0.500 | 324 |
| 300-310 deg | 30-35 deg | 0.360 | 323 |
| 330-340 deg | 35-40 deg | 0.610 | 298 |
| 100-110 deg | 10-15 deg | 0.580 | 274 |

## Clear Sky Zones (shade factor > 0.95)

| Azimuth | Elevation | Shade Factor | Energy (kWh) |
|--------:|----------:|:------------:|:------------:|
| 10-20 deg | 30-35 deg | 0.970 | 1340 |
| 70-80 deg | 60-65 deg | 0.993 | 1048 |
| 50-60 deg | 70-75 deg | 0.998 | 838 |
| 80-90 deg | 50-55 deg | 0.988 | 836 |
| 50-60 deg | 30-35 deg | 1.030 | 811 |
| 40-50 deg | 20-25 deg | 1.055 | 794 |
| 10-20 deg | 35-40 deg | 0.989 | 793 |
| 20-30 deg | 50-55 deg | 0.991 | 784 |
| 30-40 deg | 35-40 deg | 1.020 | 778 |
| 340-350 deg | 75-80 deg | 1.023 | 777 |

## Interpretation

- **Shade factor < 0.80**: Significant shading -- trees, hills, or buildings block the sun at this position
- **Shade factor 0.80-0.90**: Expected derating (inverter loss, temperature, wiring, soiling)
- **Shade factor > 0.95**: Panels perform at or above OM prediction (possible albedo/reflection boost)

## Output

Calibrated hourly estimates: `data/calibrated_solar_hourly_v2.csv` (32785 hours)

## Charts

- `charts/solar_shading_model.png` -- Polar shading map, grid, azimuth/elevation profiles, residuals, scatter