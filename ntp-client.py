import socket
import struct
import datetime
import threading
import time

NTP_SERVER = '192.168.88.201'
NTP_PORT = 123
TIME1970 = 2208988800  # January 1, 1970, 00:00:00 UTC in NTP timestamp format


def recv_worker():
    print("recv_worker started")

    # Receive the response
    data, addr = client_socket.recvfrom(48)

    # Unpack the NTP response packet
    # The transmit timestamp is located at bytes 40-47
    # '>II' indicates two unsigned integers in big-endian format
    unpacked_data = struct.unpack('>IIIIIIIIIIII', data)
    ntp_timestamp = unpacked_data[10]  # Transmit Timestamp (integer part)
    frac = unpacked_data[11]

    # Convert NTP timestamp to Unix timestamp
    unix_timestamp = ntp_timestamp - TIME1970

    # Convert Unix timestamp to a human-readable datetime object
    dt_object = datetime.datetime.fromtimestamp(unix_timestamp, tz=datetime.timezone.utc)
    print(f"Manual NTP time: {dt_object} {frac}")


if __name__ == "__main__":
    # Create a UDP socket
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    assert client_socket is not None
    client_socket.settimeout(5)  # Set a timeout for receiving data

    # thread = threading.Thread(target=recv_worker)
    # thread.start()
    # time.sleep(1)

    try:
        # Construct the NTP request packet (48 bytes, mostly zeros with a specific header)
        # Here, LI=0 (no warning), VN=4, Mode=3 (client).
        # 00 100 011
        ntp_request = b'\x23' + 47 * b'\0'

        # Send the request
        client_socket.sendto(ntp_request, (NTP_SERVER, NTP_PORT))

        data, addr = client_socket.recvfrom(48)

        # Unpack the NTP response packet
        # The transmit timestamp is located at bytes 40-47
        # '>II' indicates two unsigned integers in big-endian format
        unpacked_data = struct.unpack('>IIIIIIIIIIII', data)
        ntp_timestamp = unpacked_data[10]  # Transmit Timestamp (integer part)
        frac = unpacked_data[11] / 2 ** 32

        # Convert NTP timestamp to Unix timestamp
        unix_timestamp = ntp_timestamp - TIME1970

        # Convert Unix timestamp to a human-readable datetime object
        dt_object = datetime.datetime.fromtimestamp(unix_timestamp, tz=datetime.timezone.utc)
        print(f"Manual NTP time: {dt_object} {frac}")

    except socket.timeout:
        print("Error: NTP server did not respond within the timeout.")
    except Exception as e:
        print(f"An error occurred: {e}")

    # thread.join()
    client_socket.close()
