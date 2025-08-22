import tkinter as tk
from tkinter import filedialog, messagebox
import socket
from PIL import Image
import struct
import os

ESP32_MAC = "a0:a3:b3:ab:11:52"  # Replace with your ESP32's MAC
PORT = 1
CHUNK_SIZE = 256
BUFFER_SIZE = 32768  # 32KB buffer
TIMEOUT = 60
MAX_FRAMES = 32

def convert_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert_gif_to_binary(file_path):
    try:
        with Image.open(file_path) as img:
            if img.format != 'GIF':
                raise ValueError("Not a GIF file")

            frames = []
            num_frames = min(img.n_frames, MAX_FRAMES)

            for idx in range(num_frames):
                img.seek(idx)
                frame = img.convert("RGB").resize((64, 64))
                frame_data = bytearray()

                for y in range(64):
                    for x in range(64):
                        r, g, b = frame.getpixel((x, y))
                        rgb565 = convert_to_rgb565(r, g, b)
                        frame_data += struct.pack('<H', rgb565)

                frames.append(frame_data)

            return b''.join(frames), num_frames

    except Exception as e:
        print(f"Conversion error: {str(e)}")
        return None, 0

def send_file_frames(file_id, is_gif, file_path):
    try:
        sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        sock.settimeout(TIMEOUT)
        sock.connect((ESP32_MAC, PORT))
        print("Connected to ESP32")

        # Send upload command, file ID, and file type
        sock.sendall(b'U')
        sock.sendall(struct.pack('<H', file_id))
        sock.sendall(struct.pack('<B', 1 if is_gif else 0))

        if is_gif:
            # For GIFs, convert and send all frames at once
            gif_data, num_frames = convert_gif_to_binary(file_path)
            if not gif_data:
                raise ValueError("GIF conversion failed")
            
            # Send number of frames
            sock.sendall(struct.pack('<H', num_frames))
            print(f"Sending GIF with {num_frames} frames")

            # Send all data in chunks
            total_size = len(gif_data)
            sent = 0
            sent_since_last_ack = 0

            while sent < total_size:
                chunk = gif_data[sent:sent + CHUNK_SIZE]
                sock.sendall(chunk)
                sent += len(chunk)
                sent_since_last_ack += len(chunk)

                # Wait for acknowledgment after buffer size is reached
                if sent_since_last_ack >= BUFFER_SIZE or sent == total_size:
                    ack = sock.recv(1)
                    if ack != b'A':
                        raise RuntimeError("Acknowledgment failed")
                    sent_since_last_ack = 0
                    print(f"Sent {sent}/{total_size} bytes")
        else:
            # For single images, handle as before
            img = Image.open(file_path)
            num_frames = 1
            sock.sendall(struct.pack('<H', num_frames))

            frame_img = img.convert("RGB").resize((64, 64))
            frame_data = bytearray()

            for y in range(64):
                for x in range(64):
                    r, g, b = frame_img.getpixel((x, y))
                    color = convert_to_rgb565(r, g, b)
                    frame_data += struct.pack('<H', color)

            # Send frame data in chunks
            total_sent = 0
            frame_size = len(frame_data)
            while total_sent < frame_size:
                chunk = frame_data[total_sent:total_sent + CHUNK_SIZE]
                sent = sock.send(chunk)
                if sent == 0:
                    raise RuntimeError("Connection failed")
                total_sent += sent

            # Wait for acknowledgment
            ack = sock.recv(1)
            if ack != b'A':
                raise RuntimeError("Acknowledgment failed")

        print("All frames sent successfully!")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()
        print("Connection closed")

# [Rest of your GUI and other functions remain the same]
# send_text(), send_command(), upload_file(), send_text_command(), 
# delete_file(), run_file() remain unchanged
def send_text(file_id, text):
    try:
        sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        sock.connect((ESP32_MAC, PORT))
        print("Connected to ESP32")

        # Send text command
        sock.sendall(b'T')

        # Send file ID (2 bytes)
        sock.sendall(struct.pack('<H', file_id))

        # Send text length (2 bytes)
        text_length = len(text)
        sock.sendall(struct.pack('<H', text_length))

        # Send text data
        sock.sendall(text.encode())
        print("Text sent: " + text)

        # Wait for acknowledgment
        ack = sock.recv(1)
        if ack != b'A':
            raise RuntimeError("Acknowledgment failed")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()
        print("Connection closed")

def send_command(command, file_id=0):
    try:
        sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        sock.connect((ESP32_MAC, PORT))
        print("Connected to ESP32")
        sock.sendall(command.encode())
        if command == 'D' or command == 'R':
            sock.sendall(struct.pack('<H', file_id))
        sock.close()
        print("Command sent successfully!")
    except Exception as e:
        print(f"Error: {e}")

def upload_file():
    file_path = filedialog.askopenfilename(filetypes=[("Image/GIF Files", "*.png *.jpg *.gif")])
    if file_path:
        file_id = int(file_id_entry.get())
        is_gif = file_path.endswith(".gif")
        send_file_frames(file_id, is_gif, file_path)
        messagebox.showinfo("Success", "File uploaded and running!")

def send_text_command():
    file_id = int(file_id_entry.get())
    text = text_entry.get()
    if text:
        send_text(file_id, text)
        messagebox.showinfo("Success", "Text sent and displayed!")

def delete_file():
    file_id = int(file_id_entry.get())
    send_command('D', file_id)
    messagebox.showinfo("Success", "File deleted!")

def run_file():
    file_id = int(file_id_entry.get())
    send_command('R', file_id)
    messagebox.showinfo("Success", "File running!")

# GUI Setup
root = tk.Tk()
root.title("ESP32 LED Matrix Controller")

tk.Label(root, text="File ID:").grid(row=0, column=0)
file_id_entry = tk.Entry(root)
file_id_entry.grid(row=0, column=1)

tk.Button(root, text="Upload", command=upload_file).grid(row=1, column=0, columnspan=2)
tk.Button(root, text="Run", command=run_file).grid(row=2, column=0, columnspan=2)
tk.Button(root, text="Delete", command=delete_file).grid(row=3, column=0, columnspan=2)

tk.Label(root, text="Enter Text:").grid(row=4, column=0)
text_entry = tk.Entry(root)
text_entry.grid(row=4, column=1)

tk.Button(root, text="Send Text", command=send_text_command).grid(row=5, column=0, columnspan=2)

root.mainloop()
