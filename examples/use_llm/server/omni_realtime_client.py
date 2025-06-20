# -- coding: utf-8 --

import asyncio
import websockets
import json
import base64
import os
import time

from typing import Optional, Callable, List, Dict, Any
from enum import Enum

class TurnDetectionMode(Enum):
    SERVER_VAD = "server_vad"
    MANUAL = "manual"

class OmniRealtimeClient:
    """
    A demo client for interacting with the Omni Realtime API.

    This class provides methods to connect to the Realtime API, send text and audio data,
    handle responses, and manage the WebSocket connection.

    Attributes:
        base_url (str):
            The base URL for the Realtime API.
        api_key (str):
            The API key for authentication.
        model (str):
            Omni model to use for chat.
        voice (str):
            The voice to use for audio output.
        turn_detection_mode (TurnDetectionMode):
            The mode for turn detection.
        on_text_delta (Callable[[str], None]):
            Callback for text delta events.
            Takes in a string and returns nothing.
        on_audio_delta (Callable[[bytes], None]):
            Callback for audio delta events.
            Takes in bytes and returns nothing.
        on_input_transcript (Callable[[str], None]):
            Callback for input transcript events.
            Takes in a string and returns nothing.
        on_interrupt (Callable[[], None]):
            Callback for user interrupt events, should be used to stop audio playback.
        on_output_transcript (Callable[[str], None]):
            Callback for output transcript events.
            Takes in a string and returns nothing.
        extra_event_handlers (Dict[str, Callable[[Dict[str, Any]], None]]):
            Additional event handlers.
            Is a mapping of event names to functions that process the event payload.
    """
    def __init__(
        self,
        base_url,
        api_key: str,
        model: str = "",
        voice: str = "Chelsie",
        turn_detection_mode: TurnDetectionMode = TurnDetectionMode.MANUAL,
        on_text_delta: Optional[Callable[[str], None]] = None,
        on_audio_delta: Optional[Callable[[bytes], None]] = None,
        on_interrupt: Optional[Callable[[], None]] = None,
        on_input_transcript: Optional[Callable[[str], None]] = None,
        on_output_transcript: Optional[Callable[[str], None]] = None,
        extra_event_handlers: Optional[Dict[str, Callable[[Dict[str, Any]], None]]] = None
    ):
        self.base_url = base_url
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.ws = None
        self.on_text_delta = on_text_delta
        self.on_audio_delta = on_audio_delta
        self.on_interrupt = on_interrupt
        self.on_input_transcript = on_input_transcript
        self.on_output_transcript = on_output_transcript
        self.turn_detection_mode = turn_detection_mode
        self.extra_event_handlers = extra_event_handlers or {}

        # Track current response state
        self._current_response_id = None
        self._current_item_id = None
        self._is_responding = False
        # Track printing state for input and output transcripts
        self._print_input_transcript = False
        self._output_transcript_buffer = ""

    async def connect(self) -> None:
        """Establish WebSocket connection with the Realtime API."""
        url = f"{self.base_url}?model={self.model}"
        headers = {
            "Authorization": f"Bearer {self.api_key}"
        }
        print(f"url: {url}, headers: {headers}")

        self.ws = await websockets.connect(url, extra_headers=headers)

        # Set up default session configuration
        if self.turn_detection_mode == TurnDetectionMode.MANUAL:
            await self.update_session({
                "modalities": ["text", "audio"],
                "voice": self.voice,
                "input_audio_format": "pcm16",
                "output_audio_format": "pcm16",
                "input_audio_transcription": {
                    "model": "gummy-realtime-v1"
                },
                "turn_detection" : None
            })
        elif self.turn_detection_mode == TurnDetectionMode.SERVER_VAD:
            await self.update_session({
                "modalities": ["text", "audio"],
                "voice": self.voice,
                "input_audio_format": "pcm16",
                "output_audio_format": "pcm16",
                "input_audio_transcription": {
                    "model": "gummy-realtime-v1"
                },
                "turn_detection": {
                    "type": "server_vad",
                    "threshold": 0.1,
                    "prefix_padding_ms": 500,
                    "silence_duration_ms": 900
                }
            })
        else:
            raise ValueError(f"Invalid turn detection mode: {self.turn_detection_mode}")

    async def send_event(self, event) -> None:
        event['event_id'] = "event_" + str(int(time.time() * 1000))
        print(f" Send event: type={event['type']}, event_id={event['event_id']}")
        await self.ws.send(json.dumps(event))

    async def update_session(self, config: Dict[str, Any]) -> None:
        """Update session configuration."""
        event = {
            "type": "session.update",
            "session": config
        }
        print("update session: ", event)
        await self.send_event(event)

    async def stream_audio(self, audio_chunk: bytes) -> None:
        """Stream raw audio data to the API."""
        # only support 16bit 16kHz mono pcm
        audio_b64 = base64.b64encode(audio_chunk).decode()

        append_event = {
            "type": "input_audio_buffer.append",
            "audio": audio_b64
        }
        await self.send_event(append_event)

    async def create_response(self) -> None:
        """Request a response from the API. Needed when using manual mode."""
        event = {
            "type": "response.create",
            "response": {
                "instructions": "你是一个友好的AI助手，可以回答各种问题并进行对话。请用简洁友好的方式回复。",
                "modalities": ["text", "audio"]
            }
        }
        print("create response: ", event)
        await self.send_event(event)

    async def cancel_response(self) -> None:
        """Cancel the current response."""
        event = {
            "type": "response.cancel"
        }
        await self.send_event(event)

    async def handle_interruption(self):
        """Handle user interruption of the current response."""
        if not self._is_responding:
            return

        print(" Handling interruption")

        # 1. Cancel the current response
        if self._current_response_id:
            await self.cancel_response()

        self._is_responding = False
        self._current_response_id = None
        self._current_item_id = None

    async def handle_messages(self) -> None:
        try:
            async for message in self.ws:
                event = json.loads(message)
                event_type = event.get("type")
                
                if event_type != "response.audio.delta":
                    print(" event: ", event)
                else:
                    print(" event_type: ", event_type)

                if event_type == "error":
                    print(" Error: ", event['error'])
                    continue
                elif event_type == "response.created":
                    self._current_response_id = event.get("response", {}).get("id")
                    self._is_responding = True
                elif event_type == "response.output_item.added":
                    self._current_item_id = event.get("item", {}).get("id")
                elif event_type == "response.done":
                    self._is_responding = False
                    self._current_response_id = None
                    self._current_item_id = None
                # Handle interruptions
                elif event_type == "input_audio_buffer.speech_started":
                    print(" Speech detected")
                    if self._is_responding:
                        print(" Handling interruption")
                        await self.handle_interruption()

                    if self.on_interrupt:
                        print(" Handling on_interrupt, stop playback")
                        self.on_interrupt()
                elif event_type == "input_audio_buffer.speech_stopped":
                    print(" Speech ended")
                # Handle normal response events
                elif event_type == "response.text.delta":
                    if self.on_text_delta:
                        self.on_text_delta(event["delta"])
                elif event_type == "response.audio.delta":
                    if self.on_audio_delta:
                        audio_bytes = base64.b64decode(event["delta"])
                        self.on_audio_delta(audio_bytes)
                elif event_type == "conversation.item.input_audio_transcription.completed":
                    transcript = event.get("transcript", "")
                    if self.on_input_transcript:
                        await asyncio.to_thread(self.on_input_transcript,transcript)
                        self._print_input_transcript = True
                elif event_type == "response.audio_transcript.delta":
                    if self.on_output_transcript:
                        delta = event.get("delta", "")
                        if not self._print_input_transcript:
                            self._output_transcript_buffer += delta
                        else:
                            if self._output_transcript_buffer:
                                await asyncio.to_thread(self.on_output_transcript,self._output_transcript_buffer)
                                self._output_transcript_buffer = ""
                            await asyncio.to_thread(self.on_output_transcript,delta)
                elif event_type == "response.audio_transcript.done":
                    self._print_input_transcript = False
                elif event_type in self.extra_event_handlers:
                    self.extra_event_handlers[event_type](event)

        except websockets.exceptions.ConnectionClosed:
            print(" Connection closed")
        except Exception as e:
            print(" Error in message handling: ", str(e))

    async def close(self) -> None:
        """Close the WebSocket connection."""
        if self.ws:
            await self.ws.close()