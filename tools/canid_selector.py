import argparse
import sys

def filter_can_log(input_file, output_file, can_ids):
    """
    Reads input_file and writes to output_file only lines where the CAN ID is in can_ids.
    """
    # Normalize CAN IDs to uppercase for comparison
    can_ids = set(cid.upper() for cid in can_ids)
    
    try:
        with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
            for line in infile:
                # Based on candump.log format: (timestamp) interface ID#data
                # e.g., (1569048709.655053) can0 76C#0A0F0000696E6974
                parts = line.split()
                if len(parts) >= 3:
                    # parts[2] should be ID#data
                    id_part = parts[2].split('#')[0]
                    if id_part.upper() in can_ids:
                        outfile.write(line)
    except FileNotFoundError:
        print(f"Error: File {input_file} not found.", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"An error occurred: {e}", file=sys.stderr)
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description='Filter CAN log file by CAN IDs.')
    parser.add_argument('input', help='Input log filename')
    parser.add_argument('output', help='Output log filename')
    parser.add_argument('ids', nargs='+', help='List of CAN IDs to keep (hex format, e.g. 76C 708)')

    args = parser.parse_args()

    filter_can_log(args.input, args.output, args.ids)
    print(f"Filtering complete. Results saved to {args.output}")

if __name__ == "__main__":
    main()
