# Third-party code: Sensirion Gas Index Algorithm

`sensirion_gas_index_algorithm.c`/`.h` in this directory are vendored
**verbatim, unmodified** (only the `#include` path in the `.c` file was
adjusted from a flat `"sensirion_gas_index_algorithm.h"` to this repo's
per-component `"sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.h"`
convention -- no algorithm/logic line was touched) from:

- **Source**: <https://github.com/Sensirion/gas-index-algorithm>
- **File**: `sensirion_gas_index_algorithm_fixpoint/sensirion_gas_index_algorithm.{c,h}`
  (the **fixed-point** variant -- deliberately chosen over the floating-point
  one in the same repo, since this firmware never puts a float on the wire or
  in `sensor_channel_reading_t`; see components/sensor_channel).
- **Version**: `3.2.0` (`LIBRARY_VERSION_NAME` in the vendored header),
  fetched 2026-09-13.
- **License**: BSD-3-Clause, copyright (c) 2021/2022 Sensirion AG. The full
  license text is reproduced in `LICENSE` in this directory (from the
  repository's own top-level `LICENSE` file) and the same notice is retained
  verbatim at the top of both vendored source files, per the license's own
  redistribution terms.

## Why this is here

BH1750 and SHT45 need no comparable third-party numerics --
their conversion formulas are short, published datasheet arithmetic,
reimplemented directly in `components/bh1750`/`components/sht45`. SGP40/SGP41
(section 10) are different: the "VOC Index"/"NOx Index" values are not a
simple formula over the raw ADC ticks -- they come from a specific, tuned,
stateful signal-processing pipeline (moving mean/variance estimation, gating,
an adaptive lowpass, a scaled sigmoid) that Sensirion designed, tuned, and
publishes as reference code specifically so every integration produces the
same index for the same raw signal. Reimplementing that from a written
description would risk quietly not matching Sensirion's own numbers. Keeping
the upstream implementation and its license makes the result reproducible and
keeps the source of the algorithm clear.

## Who calls this

`components/sgp40/sgp40.c` and `components/sgp41/sgp41.c` each own one
`GasIndexAlgorithmParams` instance (VOC for both; SGP41 owns a second one for
NOx), calling `GasIndexAlgorithm_init()` once at driver init and
`GasIndexAlgorithm_process()` once per raw reading. Neither driver modifies
any tuning parameter (`GasIndexAlgorithm_set_tuning_parameters()` is never
called) -- both run with Sensirion's own published defaults.
