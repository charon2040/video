import asyncio
import base64
from datetime import datetime

import websockets
import uvicorn
from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from typing import Dict, List, Optional, Tuple
import socket
import threading
import json
import struct
import queue
import time


# 用户类
class User:
    def __init__(self, client_id: str, username: str, websocket: WebSocket):
        self.client_id = client_id
        self.username = username
        self.websocket = websocket

# 会议类
class Meeting:
    def __init__(self, meeting_id: str, creator: User, create_time: str = None):
        self.meeting_id = meeting_id
        self.creator = creator  # 会议创建者
        self.participants: Dict[str, User] = {creator.client_id: creator}  # 存储所有参会者{client_id: User}
        self.create_time = create_time or datetime.now().strftime("%Y-%m-%d %H:%M")  # 默认使用当前时间
        self.screen_sharing_client: Optional[str] = None  # 记录当前共享屏幕的客户端ID

    def add_participant(self, user: User):
        # 添加参会者
        self.participants[user.client_id] = user

    def remove_participant(self, client_id: str) -> Optional[User]:
        # 移除参会者并返回被移除的用户
        user = self.participants.pop(client_id, None)
        if user and self.screen_sharing_client == client_id:
            self.screen_sharing_client = None  # 清除共享状态
        return user


    def get_participant_list(self) -> List[Dict[str, str]]:
        # 返回参会者列表，包含 client_id 和 username
        return [
            {"client_id": client_id, "username": user.username}
            for client_id, user in self.participants.items()
        ]

    def get_creator(self) -> User:
        # 返回会议创建者
        return self.creator


app = FastAPI()

# 存储所有会议 {meeting_id: Meeting}
meetings: Dict[str, Meeting] = {}

# 存储 client_id 和 username 的映射关系
client_usernames: Dict[str, str] = {}

# 存储Unity渲染客户端 - 分离视频和音频
video_render_clients: Dict[str, socket.socket] = {}
audio_render_clients: Dict[str, socket.socket] = {}

# 帧缓冲区，分离存储
video_buffers: Dict[str, Tuple[bytes, int, int]] = {}
audio_buffers: Dict[str, bytes] = {}

# 用于主事件循环与TCP线程通信的队列
video_queue = queue.Queue()
audio_queue = queue.Queue()

# 统计相关变量
video_frame_count = 0
audio_packet_count = 0
last_stats_time = time.time()


# 广播文本消息给指定会议室的客户端
async def broadcast_message(meeting_id: str, message: str, exclude_client: Optional[str] = None):
    if meeting_id in meetings:
        disconnected_clients = []
        for client_id, user in meetings[meeting_id].participants.items():
            if client_id != exclude_client:
                try:
                    await user.websocket.send_text(message)
                except Exception as e:
                    print(f"Error sending to video client {client_id}: {e}")
                    disconnected_clients.append(client_id)
        for client_id in disconnected_clients:
            meetings[meeting_id].remove_participant(client_id)
        if not meetings[meeting_id].participants:
            del meetings[meeting_id]

@app.websocket("/ws/video/{client_id}")
async def video_websocket_endpoint(websocket: WebSocket, client_id: str):
    await websocket.accept()
    try:
        while True:
            message = await websocket.receive()
            if 'text' in message:
                text_data = message['text']

                try:
                    msg = json.loads(text_data)
                    msg_type = msg.get('type')
                    meeting_id = msg.get('meeting_id', '')
                    m_client_id = msg.get('client_id')

                    # 获取用户名
                    username = msg.get('username')
                    # 更新或设置 client_id 和 username 的映射
                    if username is not None:
                        client_usernames[client_id] = username

                    username = client_usernames.get(client_id, "Anonymous")

                    if m_client_id != client_id:
                        print(f"接受到异常消息, m_client_id: {m_client_id}, client_id: {client_id}")
                        continue

                    if msg_type == "create_meeting":
                        if not meeting_id or meeting_id in meetings:
                            await websocket.send_text(json.dumps({
                                "status": "error",
                                "message": "Invalid or duplicate meeting ID"
                            }))
                        else:
                            # 创建用户和会议
                            create_time = msg.get('create_time', datetime.now().strftime("%Y-%m-%d %H:%M"))
                            user = User(client_id, username, websocket)
                            meeting = Meeting(meeting_id, user, create_time)
                            meetings[meeting_id] = meeting
                            await websocket.send_text(json.dumps({
                                "status": "success",
                                "message": f"Meeting {meeting_id} created",
                                "meeting_id": meeting_id,
                                "create_time": create_time
                            }))

                    elif msg_type == "join_meeting":
                        if not meeting_id or meeting_id not in meetings:
                            await websocket.send_text(json.dumps({
                                "status": "error",
                                "message": "Meeting ID not found"
                            }))
                        else:
                            # 创建新用户并加入会议
                            user = User(client_id, username, websocket)
                            meetings[meeting_id].add_participant(user)
                            # 向新用户发送会议详情
                            creator = meetings[meeting_id].get_creator()
                            participants = meetings[meeting_id].get_participant_list()
                            response = {
                                "status": "success",
                                "message": f"Joined meeting {meeting_id}",
                                "meeting_id": meeting_id,
                                "creator_id": creator.client_id,
                                "creator_username": creator.username,
                                "participants": participants,
                                "create_time": meetings[meeting_id].create_time
                            }
                            # 如果有人在共享屏幕，通知新加入的客户端
                            if meetings[meeting_id].screen_sharing_client:
                                response["screen_sharing_client"] = meetings[meeting_id].screen_sharing_client
                            await websocket.send_text(json.dumps(response))
                            #向所有参会者广播新用户加入
                            join_message = {
                                "status": "success",
                                "message": f"User {client_id} joined",
                                "client_id": client_id,
                                "username": username,
                                "meeting_id": meeting_id
                            }
                            await broadcast_message(meeting_id, json.dumps(join_message), exclude_client=client_id)

                    elif msg_type == "leave_meeting":
                        if meeting_id in meetings and client_id in meetings[meeting_id].participants:
                            user = meetings[meeting_id].remove_participant(client_id)
                            if not meetings[meeting_id].participants:
                                del meetings[meeting_id]
                            else:
                                if user:
                                    await broadcast_message(meeting_id, json.dumps({
                                        "status": "success",
                                        "message": f"用户 {user.username} 离开了会议",
                                        "client_id": client_id,
                                        "username": user.username
                                    }))

                    elif msg_type == "user_speech":
                        content = msg.get('content', '无内容')
                        if meeting_id in meetings and client_id in meetings[meeting_id].participants:
                            # 构造 JSON 消息
                            speech_message = {
                                "status": "success",
                                "type": "user_speech",
                                "client_id": client_id,
                                "username": username,
                                "content": content,
                                "meeting_id": meeting_id
                            }
                            await broadcast_message(meeting_id, json.dumps(speech_message), exclude_client=client_id)

                    else:
                        print(f"未知消息类型: {msg_type}")
                except json.JSONDecodeError:
                    print(f"无效的JSON消息: {text_data}")
    except Exception as e:
        print(f"Video client {client_id} disconnected: {e}")
    finally:
        for mid, meeting in list(meetings.items()):
            if client_id in meeting.participants:
                user = meeting.remove_participant(client_id)
                if not meeting.participants:
                    del meetings[mid]
                else:
                    if user:
                        await broadcast_message(mid, json.dumps({
                            "status": "success",
                            "message": f"用户 {user.username} 离开了会议",
                            "client_id": client_id,
                            "username": user.username
                        }))
                break


# 统计函数
def update_stats():
    global video_frame_count, audio_packet_count, last_stats_time

    current_time = time.time()
    if current_time - last_stats_time >= 3.0:
        elapsed = current_time - last_stats_time

        video_fps = video_frame_count / elapsed if elapsed > 0 else 0
        audio_pps = audio_packet_count / elapsed if elapsed > 0 else 0

        print(f"\n[统计信息] 视频: {video_fps:.1f}FPS, 音频: {audio_pps:.1f}PPS")
        print(f"[客户端] Unity视频: {len(video_render_clients)}, Unity音频: {len(audio_render_clients)}")
        print(f"[会议] 当前会议数: {len(meetings)}\n")

        # 重置计数器
        video_frame_count = 0
        audio_packet_count = 0
        last_stats_time = current_time


async def broadcast_video_frame(client_id: str, frame: bytes, width: int, height: int):
    # 保存视频帧
    video_buffers[client_id] = (frame, width, height)
    
    # 转发给所有Unity视频客户端
    if video_render_clients:
        disconnected_clients = []
        for unity_id, unity_socket in video_render_clients.items():
            try:
                send_video_frame_tcp(unity_socket, frame, width, height)
            except Exception as e:
                print(f"[视频] 发送到Unity {unity_id} 失败: {e}")
                disconnected_clients.append(unity_id)
        
        # 清理断开的客户端
        for unity_id in disconnected_clients:
            video_render_clients.pop(unity_id, None)


async def broadcast_audio_data(client_id: str, audio_data: bytes):
    # 保存音频数据
    audio_buffers[client_id] = audio_data
    
    # 转发给所有Unity音频客户端
    if audio_render_clients:
        disconnected_clients = []
        for unity_id, unity_socket in audio_render_clients.items():
            try:
                send_audio_data_tcp(unity_socket, audio_data)
            except Exception as e:
                print(f"[音频] 发送到Unity {unity_id} 失败: {e}")
                disconnected_clients.append(unity_id)
        
        # 清理断开的客户端
        for unity_id in disconnected_clients:
            audio_render_clients.pop(unity_id, None)


def send_video_frame_tcp(client_socket: socket.socket, frame_data: bytes, width: int, height: int):
    """发送视频帧到Unity"""
    try:
        magic = 0x12345678
        header_size = 24
        payload_size = len(frame_data)
        timestamp = int(time.time() * 1000)

        header = struct.pack('<IIIHHQ',
                             magic, header_size, payload_size,
                             width, height, timestamp)

        client_socket.sendall(header)
        client_socket.sendall(frame_data)

    except Exception as e:
        raise Exception(f"TCP视频发送失败: {e}")


def send_audio_data_tcp(client_socket: socket.socket, audio_data: bytes):
    """发送音频数据到Unity"""
    try:
        magic = 0x87654321
        header_size = 20
        payload_size = len(audio_data)
        timestamp = int(time.time() * 1000)

        header = struct.pack('<IIIQ',
                             magic, header_size, payload_size, timestamp)

        client_socket.sendall(header)
        client_socket.sendall(audio_data)

    except Exception as e:
        raise Exception(f"TCP音频发送失败: {e}")


# 处理队列中的数据
async def process_queues():
    while True:
        # 处理视频队列
        if not video_queue.empty():
            client_id, frame_data, width, height = video_queue.get()
            await broadcast_video_frame(client_id, frame_data, width, height)
        
        # 处理音频队列
        if not audio_queue.empty():
            client_id, audio_data = audio_queue.get()
            await broadcast_audio_data(client_id, audio_data)
        
        # 更新统计
        update_stats()
            
        await asyncio.sleep(0.01)


# TCP视频服务器线程（端口8888）
def video_tcp_server_thread():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(('0.0.0.0', 8888))
    server_socket.listen(5)
    print("视频TCP服务器启动在端口8888")

    while True:
        client_socket, client_address = server_socket.accept()
        print(f"新的C++视频客户端连接: {client_address}")
        threading.Thread(target=handle_video_client, args=(client_socket,), daemon=True).start()


# TCP音频服务器线程（端口8890）
def audio_tcp_server_thread():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_socket.bind(('0.0.0.0', 8890))
        server_socket.listen(5)
        print("音频TCP服务器启动在端口8890")
    except Exception as e:
        print(f"音频TCP服务器启动失败: {e}")
        return

    while True:
        try:
            client_socket, client_address = server_socket.accept()
            print(f"新的C++音频客户端连接: {client_address}")
            threading.Thread(target=handle_audio_client, args=(client_socket,), daemon=True).start()
        except Exception as e:
            print(f"音频服务器接受连接错误: {e}")


# Unity视频TCP服务器线程（端口8889）
def unity_video_tcp_server_thread():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(('0.0.0.0', 8889))
    server_socket.listen(5)
    print("Unity视频TCP服务器启动在端口8889")

    while True:
        client_socket, client_address = server_socket.accept()
        unity_client_id = f"unity_video_{client_address[1]}"
        video_render_clients[unity_client_id] = client_socket
        print(f"Unity视频客户端连接: {unity_client_id}")
        threading.Thread(target=handle_unity_video_client, args=(client_socket, unity_client_id), daemon=True).start()


# Unity音频TCP服务器线程（端口8891）
def unity_audio_tcp_server_thread():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(('0.0.0.0', 8891))
    server_socket.listen(5)
    print("Unity音频TCP服务器启动在端口8891")

    while True:
        client_socket, client_address = server_socket.accept()
        unity_client_id = f"unity_audio_{client_address[1]}"
        audio_render_clients[unity_client_id] = client_socket
        print(f"Unity音频客户端连接: {unity_client_id}")
        threading.Thread(target=handle_unity_audio_client, args=(client_socket, unity_client_id), daemon=True).start()


def handle_video_client(client_socket: socket.socket):
    """处理C++视频客户端"""
    try:
        client_id = f"video_tcp_{client_socket.getpeername()[1]}"

        while True:
            # 接收视频帧头（24字节）
            header_data = recvall(client_socket, 24)
            if not header_data or len(header_data) != 24:
                break

            # 解析视频帧头
            header = parse_video_header(header_data)
            if not header:
                break

            # 接收视频帧数据
            frame_data = recvall(client_socket, header['payload_size'])
            if not frame_data or len(frame_data) != header['payload_size']:
                break

            # 统计并放入队列
            global video_frame_count
            video_frame_count += 1
            video_queue.put((client_id, frame_data, header['width'], header['height']))

    except Exception as e:
        print(f"视频客户端处理错误: {e}")
    finally:
        client_socket.close()
        if client_id in video_buffers:
            del video_buffers[client_id]


def handle_audio_client(client_socket: socket.socket):
    """处理C++音频客户端"""
    try:
        client_id = f"audio_tcp_{client_socket.getpeername()[1]}"
        print(f"开始处理音频客户端: {client_id}")
        
        # 音频帧头大小：4+4+4+8=20字节 (紧密对齐)
        AUDIO_HEADER_SIZE = 20
        
        while True:
            # 接收音频帧头（20字节）
            header_data = recvall(client_socket, AUDIO_HEADER_SIZE)
            if not header_data or len(header_data) != AUDIO_HEADER_SIZE:
                print(f"[{client_id}] 音频帧头接收失败")
                break
            
            # 解析音频帧头
            header = parse_audio_header(header_data)
            if not header:
                print(f"[{client_id}] 音频帧头解析失败")
                break
            
            # 接收音频数据
            audio_data = recvall(client_socket, header['payload_size'])
            if not audio_data or len(audio_data) != header['payload_size']:
                print(f"[{client_id}] 音频数据接收失败: 期望{header['payload_size']}, 实际{len(audio_data) if audio_data else 0}")
                break
            
            # 统计并放入队列
            global audio_packet_count
            audio_packet_count += 1
            audio_queue.put((client_id, audio_data))
            
            # 输出前几个包的调试信息
            if audio_packet_count <= 10 or audio_packet_count % 100 == 1:
                print(f"[AUDIO_TCP] 接收音频包: {audio_packet_count}, 大小: {header['payload_size']} 字节, 客户端: {client_id}")
    
    except Exception as e:
        print(f"音频客户端处理错误: {e}")
    finally:
        print(f"音频客户端 {client_id} 断开连接")
        client_socket.close()
        if client_id in audio_buffers:
            del audio_buffers[client_id]


def handle_unity_video_client(client_socket: socket.socket, client_id: str):
    """处理Unity视频客户端连接"""
    try:
        print(f"Unity视频客户端 {client_id} 已连接")

        # 发送最新的视频帧（如果有的话）
        if video_buffers:
            for tcp_client_id, (frame_data, width, height) in video_buffers.items():
                try:
                    send_video_frame_tcp(client_socket, frame_data, width, height)
                    break
                except:
                    pass

        # 保持连接
        while True:
            try:
                data = client_socket.recv(1024)
                if not data:
                    break
            except:
                break

    except Exception as e:
        print(f"Unity视频客户端 {client_id} 错误: {e}")
    finally:
        print(f"Unity视频客户端 {client_id} 断开连接")
        if client_id in video_render_clients:
            video_render_clients.pop(client_id, None)
        try:
            client_socket.close()
        except:
            pass


def handle_unity_audio_client(client_socket: socket.socket, client_id: str):
    """处理Unity音频客户端连接"""
    try:
        print(f"Unity音频客户端 {client_id} 已连接")

        # 发送最新的音频数据（如果有的话）
        if audio_buffers:
            for tcp_client_id, audio_data in audio_buffers.items():
                try:
                    send_audio_data_tcp(client_socket, audio_data)
                    break
                except:
                    pass

        # 保持连接
        while True:
            try:
                data = client_socket.recv(1024)
                if not data:
                    break
            except:
                break

    except Exception as e:
        print(f"Unity音频客户端 {client_id} 错误: {e}")
    finally:
        print(f"Unity音频客户端 {client_id} 断开连接")
        if client_id in audio_render_clients:
            audio_render_clients.pop(client_id, None)
        try:
            client_socket.close()
        except:
            pass


def parse_video_header(header_data: bytes) -> dict:
    """解析视频帧头"""
    try:
        magic = struct.unpack('<I', header_data[0:4])[0]
        if magic != 0x12345678:
            return None

        header_size = struct.unpack('<I', header_data[4:8])[0]
        payload_size = struct.unpack('<I', header_data[8:12])[0]
        width = struct.unpack('<H', header_data[12:14])[0]
        height = struct.unpack('<H', header_data[14:16])[0]
        timestamp = struct.unpack('<Q', header_data[16:24])[0]

        return {
            "magic": magic,
            "header_size": header_size,
            "payload_size": payload_size,
            "width": width,
            "height": height,
            "timestamp": timestamp
        }
    except:
        return None


def parse_audio_header(header_data: bytes) -> dict:
    """解析音频帧头"""
    try:
        magic = struct.unpack('<I', header_data[0:4])[0]
        if magic != 0x87654321:
            print(f"[AUDIO_DEBUG] 魔数不匹配: 0x{magic:08X}, 期望: 0x87654321")
            return None

        header_size = struct.unpack('<I', header_data[4:8])[0]
        payload_size = struct.unpack('<I', header_data[8:12])[0]
        timestamp = struct.unpack('<q', header_data[12:20])[0]  # 改为小写q，有符号64位整数

        print(f"[AUDIO_DEBUG] 解析音频帧头成功: magic=0x{magic:08X}, size={payload_size}, timestamp={timestamp}")
        
        return {
            "magic": magic,
            "header_size": header_size,
            "payload_size": payload_size,
            "timestamp": timestamp
        }
    except Exception as e:
        print(f"[AUDIO_DEBUG] 音频帧头解析异常: {e}")
        # 打印原始字节以便调试
        hex_data = ' '.join(f'{b:02X}' for b in header_data)
        print(f"[AUDIO_DEBUG] 原始帧头字节: {hex_data}")
        return None


def recvall(sock: socket.socket, count: int) -> bytes:
    """接收指定字节数的数据"""
    buf = b''
    while count > 0:
        new_buf = sock.recv(count)
        if not new_buf:
            return b''
        buf += new_buf
        count -= len(new_buf)
    return buf


# 启动所有TCP服务器线程
video_tcp_thread = threading.Thread(target=video_tcp_server_thread, daemon=True)
video_tcp_thread.start()

audio_tcp_thread = threading.Thread(target=audio_tcp_server_thread, daemon=True)
audio_tcp_thread.start()

unity_video_tcp_thread = threading.Thread(target=unity_video_tcp_server_thread, daemon=True)
unity_video_tcp_thread.start()

unity_audio_tcp_thread = threading.Thread(target=unity_audio_tcp_server_thread, daemon=True)
unity_audio_tcp_thread.start()


@app.on_event("startup")
async def startup_event():
    # 启动队列处理任务
    asyncio.create_task(process_queues())


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)

