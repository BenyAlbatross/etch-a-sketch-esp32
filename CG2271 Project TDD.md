Problem statement 

Etch-a-Sketch AI Pictionary modernises the classic drawing toy by integrating AI-powered image recognition into a real-time Pictionary game. Players use physical rotary encoders to draw on-screen in response to AI-generated prompts. When a drawing is submitted, a cloud vision model evaluates it and returns a guess, with the system providing immediate audiovisual feedback via buzzers and an RGB LED. 

Sensors and Actuators used 

- 2x Rotary Encoders: Trigger interrupts when movement is detected to control the X and Y-axis position of the cursor. 
    

- Button (on MCXC444): Triggers interrupt when a press is detected. Allows the user to generate a new image prompt, and to submit their drawing for a guess. 
    

- Touch Sensor: Records a tap via polling, which toggles the "pen" state (up/down), allowing the user to transition between drawing continuous lines and moving the cursor without leaving a trail. 
    

- MPU-6050 3-axis Gyroscope/Accelerometer: Records physical shaking motion to clear the screen via polling, mimicking the mechanical erase function of a traditional Etch a Sketch. 
    

- Active Buzzer: Emits a sharp, continuous warning tone when the user's cursor rotates into the boundary edges of the screen. 
    

- Passive Buzzer: Emits PWM-driven melodies, such as a victory chime for a correct AI guess or a losing sound for an incorrect one. 
    

- SMD RGB LED: Provides visual feedback on the AI's evaluation, illuminating green for a correct guess and red for an incorrect guess. 
    

General Architecture 

We plan to connect the two rotary encoders, the physical button, the touch sensor, and the MPU-6050 gyroscope to the MCXC444.  

An asynchronous, interrupt-driven UART link (9600 baud) connects the MCXC444 and ESP32. Outgoing packets carry cursor position, pen state, and command flags (e.g., $S,x,y,penDown,erase,submit\n). Incoming packets carry the AI result to trigger appropriate actuator (LED, Passive Buzzer) responses. 

ESP32 and Web Client 

The ESP32 maintains a 2D framebuffer and hosts a WebSocket server that streams JSON payloads to a local web client on a laptop for real-time rendering. The client displays the current drawing, the active prompt, and the pen state. 

Button Behaviour 

On prompt requests, the ESP32 sends a HTTPS POST request to the OpenAI/Gemini API (GPT-5.4 nano or Gemini 3 Flash) for a Pictionary noun and forwards it to the client to display “Draw a picture of XXX” on screen. 

On submission, the ESP32 encodes the framebuffer as a PNG, converts it to base64 encoding, and issues a HTTPS POST request to a vision model (GPT-5.4 nano or Gemini 3 Flash) with a structured prompt requesting a JSON response of the form {"guess": "<string>", "confidence": <1–10>}. Because the player's drawing may match the target prompt conceptually but differ in exact wording (e.g., "automobile" vs "car"), a second API call performs semantic matching between the guess and the original prompt, returning a JSON-formatted boolean correctness judgment. Based on the correctness result, the ESP32 formats a response packet and transmits it to the MCXC444 over UART. The MCXC444 then drives the RGB LED and passive buzzer accordingly. The confidence score and the correctness judgement is sent to the client WebSocket to display on screen. 

Current progress  

We have implemented the core sensing pipeline on the MCXC444, including the rotary encoders, touch sensor and gyroscope. Boundary detection logic has also been integrated. UART communication between the MCXC444 and ESP32 has been established, enabling transmission of system state data such as cursor position, pen state, and control commands. 

In the coming weeks, we aim to complete the code for the ESP32 and the websocket client. We also aim to integrate the LED into the MCXC444 system. Lastly, we will do full system integration and end-to-end testing will be conducted across the full pipeline (user input -> MCU processing -> ESP32 handling -> cloud evaluation -> feedback output).