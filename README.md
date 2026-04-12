# Orchid  IoT Monitoring System


<img width="1477" height="1108" alt="image" src="https://github.com/user-attachments/assets/cda9245a-0092-4249-ba2c-de2866586114" />

## Overview

The system combines multi-sensor data acquisition with deep learning–based image analysis to support real-time monitoring and disease assessment.

## System Components

### Hardware
- ESP32 (NodeMCU)
- Temperature & Humidity Sensor (SHT31)
- CO₂ Sensor (SenseAir S8)
- Oxygen Sensor (DFRobot)
- Light Sensor (BH1750)
- Wind Sensor (FS3000)

## Functionality

- Disease severity estimation  
- Data logging (CSV format)  

## Implementation
- Sensors are integrated via I2C and serial communication  
- Power consumption is reduced using MOSFET-based control  
- Environmental data is logged and used for further analysis

## Circuit Design
- Power control implemented using IRLZ34N MOSFET for switching sensor modules
- Reverse current protection using IN5819 Schottky diode
- Designed for low power consumption and stable operation
  
## Sensor Device

The sensing device is designed and assembled as a portable unit.
<img width="1477" height="1108" alt="image" src="https://github.com/user-attachments/assets/4fb1b0c5-cc1f-43a0-b189-866e40a537a7" />

## Multi-I2C Design

To support multiple sensors with fixed I2C addresses, a TCA9548A multiplexer is used.

- Each sensor group is assigned to a dedicated I2C channel  
- Channel switching is performed before each sensor read  
- Enables simultaneous use of identical sensors (e.g., dual FS3000)  
- Improves system scalability and stability  

Example:

```cpp
tcaSelect(TCA_CH_FS1);
tcaSelect(TCA_CH_FS2);
