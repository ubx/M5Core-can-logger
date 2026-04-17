### CAN ID Selector

`canid_selector.py` is a Python script designed to filter CAN log files (in `candump` format) based on a list of specified CAN IDs. It reads an input log file and writes only the lines containing the desired CAN IDs to an output file.

#### Usage

```bash
python3 tools/canid_selector.py <input_file> <output_file> <can_id1> [<can_id2> ...]
```

#### Arguments

- `input`: Path to the input CAN log file (e.g., `data/candump.log`).
- `output`: Path where the filtered log file will be saved.
- `ids`: One or more CAN IDs (in hex format, e.g., `76C`, `708`) to keep.

#### Example

To filter `data/candump.log` and keep only messages with IDs `708` and `76C`, saving the result to `filtered.log`:

```bash
python3 tools/canid_selector.py data/candump.log filtered.log 708 76C
```

#### Supported Format

The script expects the standard `candump` log format:
`(timestamp) interface ID#data`

Example line:
`(1569048709.655053) can0 76C#0A0F0000696E6974`
