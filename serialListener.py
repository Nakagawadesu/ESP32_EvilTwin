import serial
import time
import json

SERIAL_PORT = '/dev/ttyUSB0'
BAUD_RATE = 115200
OUTPUT_FILE = 'serial_output.txt'
PARSED_CREDENTIALS_FILE = 'parsed_credentials.txt'

def connect_serial():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"Connected to {SERIAL_PORT}")
        return ser
    except serial.SerialException as e:
        print(f"Failed to connect: {e}")
        return None

def main():
    ser = None
    while True:
        if ser is None or not ser.is_open:
            ser = connect_serial()
            if ser is None:
                time.sleep(2)  # Wait before retrying
                continue

        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                print(line)  # Print raw data to console

                with open(OUTPUT_FILE, 'a') as f_output:
                    f_output.write(line + '\n')

                if '#### Dados recebidos:' in line:
                    try:
                        # Extract the JSON part
                        json_str = line.split('#### Dados recebidos: ')[1]
                        json_data = json.loads(json_str)

                        # Extract username and password
                        username = json_data.get('username', 'N/A')
                        password = json_data.get('password', 'N/A')

                        # Format the credentials
                        credentials = f"Username: {username}, Password: {password}\n###\n"

                        # Write the credentials to the file
                        with open(PARSED_CREDENTIALS_FILE, 'a') as f_credentials:
                            f_credentials.write(credentials)

                        print("Stored credentials:", credentials.strip())
                    except (json.JSONDecodeError, IndexError) as e:
                        print(f"Error parsing JSON: {e}")
        except serial.SerialException as e:
            print(f"Serial error: {e}. Reconnecting...")
            ser.close()
            ser = None
        except KeyboardInterrupt:
            print("Stopping...")
            break

    if ser and ser.is_open:
        ser.close()

if __name__ == "__main__":
    main()