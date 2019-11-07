#include "driver.h"
#include "driver/gpio.h"
#include "xtensa/core-macros.h"
#include "mallocs.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include <string.h>
#include "esp_pm.h"
#include "vgm.h"
#include "pins.h"
#include "dacstream.h"
#include "channels.h"
#include "hal.h"
#include "ringbuf.h"

static const char* TAG = "Driver";

#define DRIVER_CLOCK_RATE 240000000 //clock rate of cpu while in playback
#define DRIVER_VGM_SAMPLE_RATE 44100
#define DRIVER_CYCLES_PER_SAMPLE (DRIVER_CLOCK_RATE/DRIVER_VGM_SAMPLE_RATE)

#if defined HWVER_PORTABLE
#define SR_CONTROL      0
#define SR_DATABUS      1
#define SR_BIT_PSG_CS   0x01 // PSG /CS
#define SR_BIT_WR       0x04 // /WR
#define SR_BIT_FM_CS    0x20 // FM /CS
#define SR_BIT_A0       0x08 // A0
#define SR_BIT_A1       0x10 // A1
#define SR_BIT_IC       0x02 // /IC
uint8_t Driver_SrBuf[2] = {0xff & ~SR_BIT_IC,0x00};
#elif defined HWVER_DESKTOP
#define SR_CONTROL      1
#define SR_DATABUS      0
#define SR_BIT_PSG_CS   0x40 // PSG /CS
#define SR_BIT_WR       0x20 // /WR
#define SR_BIT_FM_CS    0x80 // FM /CS
#define SR_BIT_A0       0x08 // A0
#define SR_BIT_A1       0x04 // A1
#define SR_BIT_IC       0x10 // /IC
uint8_t Driver_SrBuf[2] = {0x00,0xff & ~SR_BIT_IC};
#endif

volatile Ringbuf_t Driver_CommandRingbuf; //incoming vgm data
volatile Ringbuf_t Driver_PcmRingbuf; //attached pcm data (if any)
EventGroupHandle_t Driver_CommandEvents; //driver status flags
EventGroupHandle_t Driver_QueueEvents; //queue status flags
uint8_t Driver_CommandQueueBuf[DRIVER_QUEUE_SIZE];
uint8_t Driver_PcmBuf[DACSTREAM_BUF_SIZE*DACSTREAM_PRE_COUNT];

volatile uint32_t Driver_CpuPeriod = 0;
volatile uint32_t Driver_CpuUsageVgm = 0;
volatile uint32_t Driver_CpuUsageDs = 0;

volatile bool Driver_FixPsgFrequency = true;
volatile bool Driver_MitigateVgmTrim = true;

//we abuse the esp32's spi hardware to drive the shift registers
spi_bus_config_t Driver_SpiBusConfig = {
    .miso_io_num=-1,
    .mosi_io_num=PIN_DRIVER_DATA,
    .sclk_io_num=PIN_DRIVER_SHCLK,
    .quadwp_io_num=-1,
    .quadhd_io_num=-1,
};
#ifdef HWVER_PORTABLE
spi_device_interface_config_t Driver_SpiDeviceConfig = {
    .clock_speed_hz=26600000, //shift registers are spec'd at typ 52MHz @ 25C but signal integrity is too poor at 40MHz (3.3v data is probably not helping either). 26.6 is rock solid though.
    .mode=0,
    .spics_io_num=PIN_DRIVER_SHSTO,
    .queue_size=10,
};
#elif defined HWVER_DESKTOP
spi_device_interface_config_t Driver_SpiDeviceConfig = {
    .clock_speed_hz=40000000, //signal integrity at 40MHz is good as of desktop pcb ver 1.1. however, if you are breadboarding this, 40MHz is probably too hairy, so decrease this
    .mode=0,
    .spics_io_num=PIN_DRIVER_SHSTO,
    .queue_size=10,
    .flags = SPI_DEVICE_NO_DUMMY,
};
#endif
spi_device_handle_t Driver_SpiDevice;

//vgm / 2612 pcm stuff
uint32_t Driver_Sample = 0;     //current sample number
uint64_t Driver_Cycle = 0;      //current cycle number
uint32_t Driver_Cc = 0;         //current cycle from the api - just keep it off the stack
uint32_t Driver_LastCc = 0;     //copy of the above var
uint32_t Driver_NextSample = 0; //sample number at which the next command needs to be run
uint8_t Driver_FmAlgo[6] = {0,0,0,0,0,0};
uint8_t Driver_PsgLastChannel = 0;
volatile bool Driver_FirstWait = true;
uint8_t Driver_FmPans[6] = {0b11000000,0b11000000,0b11000000,0b11000000,0b11000000,0b11000000};
uint32_t Driver_PauseSample = 0; //sample no before stop
uint8_t Driver_PsgAttenuation[4] = {0x10, 0x10, 0x10, 0x10}; //garbage values, initial ones are set properly in start
bool Driver_NoLeds = false;

//dacstream specific
uint32_t DacStreamSeq = 0;              //sequence no of the current stream
uint32_t DacStreamLastSeqPlayed = 0;    //last seq successfully played
uint8_t DacStreamId = 0;                //index of the currently playing dacstream. should maybe consider using a pointer to the queue instead of keeping this around
bool DacStreamActive = false;           //actively playing a stream?
uint32_t DacStreamSampleRate = 0;       //stream current sample rate
uint32_t DacStreamSampleTime = 0;       //stream sample timer
uint8_t DacStreamPort = 0;              //chip port to write to
uint8_t DacStreamCommand = 0;           //chip command to use
uint32_t DacStreamSamplesPlayed = 0;    //how many samples played so far
uint8_t DacStreamLengthMode = 0;
uint32_t DacStreamDataLength = 0;

volatile uint8_t Driver_FmMask = 0b01111111;
volatile uint8_t Driver_PsgMask = 0b00001111;
uint8_t Driver_DacEn = 0;
volatile bool Driver_ForceMono = false;

#define min(a,b) ((a) < (b) ? (a) : (b)) //sigh.

void Driver_ResetChips();

void Driver_Output() { //output data to shift registers
    disp_spi_transfer_data(Driver_SpiDevice, (uint8_t*)&Driver_SrBuf, NULL, 2, 0);
}

bool Driver_Setup() {
    //create the ringbufs and event groups
    ESP_LOGI(TAG, "Setting up");
    Driver_CommandRingbuf.Buf = &Driver_CommandQueueBuf;
    Driver_CommandRingbuf.Size = sizeof(Driver_CommandQueueBuf);
    Ringbuf_Init(&Driver_CommandRingbuf);

    Driver_PcmRingbuf.Buf = &Driver_PcmBuf;
    Driver_PcmRingbuf.Size = sizeof(Driver_PcmBuf);
    Ringbuf_Init(&Driver_PcmRingbuf);

    Driver_CommandEvents = xEventGroupCreate();
    if (Driver_CommandEvents == NULL) {
        ESP_LOGE(TAG, "Command event group create failed !!");
        return false;
    }
    Driver_QueueEvents = xEventGroupCreate();
    if (Driver_QueueEvents == NULL) {
        ESP_LOGE(TAG, "Queue event group create failed !!");
        return false;
    }

    //setup the spi stuff
    ESP_LOGI(TAG, "Spi setup...");
    esp_err_t ret = spi_bus_initialize(VSPI_HOST, &Driver_SpiBusConfig, 0); //last param = dma ch.
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Spi bus init fail !! 0x%x", ret);
        return false;
    }
    ret = spi_bus_add_device(VSPI_HOST, &Driver_SpiDeviceConfig, &Driver_SpiDevice);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Spi bus device add fail !! 0x%x", ret);
        return false;
    }
    
    //spi configs are normally applied on the first call to spi_device_transmit
    //but we have a hacked library and we don't use that method so it would never be set up
    //output some dummy data just to get the config applied
    spi_transaction_t txn;
    memset(&txn, 0, sizeof(txn));
    txn.length = 16; //BITS !!
    txn.tx_buffer = &Driver_SrBuf;
    txn.rx_buffer = NULL;
    spi_device_transmit(Driver_SpiDevice, &txn);

    //and now output initial values
    Driver_Output();

    Driver_ResetChips();

    /*ESP_LOGW(TAG, "Benchmarking FmOut");
    uint32_t s = xthal_get_ccount();
    for (uint8_t i=0;i<255;i++) {
        Driver_FmOut(0,0,0);
    }
    uint32_t e = xthal_get_ccount();
    e = (e-s)/255;
    ESP_LOGW(TAG, "Average write latency %d clocks", e);*/

    ESP_LOGI(TAG, "Ready");
    return true;
}

void Driver_Sleep(uint32_t us) { //quick and dirty spin sleep
    uint32_t s = xthal_get_ccount();
    uint32_t c = us*(DRIVER_CLOCK_RATE/1000000);
    while (xthal_get_ccount() - s < c);
}

void Driver_PsgOut(uint8_t Data) {
    //data bus is reversed for the psg because it made pcb layout easier
    Driver_SrBuf[SR_DATABUS] = 0;
    for (uint8_t i=0;i<=7;i++) {
        Driver_SrBuf[SR_DATABUS] |= ((Data>>(7-i))&1)<<i;
    }
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_PSG_CS;
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR;
    Driver_Output();
    Driver_Sleep(20);
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR;
    Driver_Output();
    //Driver_Sleep(20);
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_PSG_CS;
    Driver_Output();
    Driver_Sleep(5);

    //channel led stuff
    if (Driver_NoLeds) return;
    if (Data & 0x80) {
        Driver_PsgLastChannel = (Data & 0b01100000) >> 5;
    }
    uint8_t ch = 6 + Driver_PsgLastChannel; //6 = psg ch offset in array
    if ((Data & 0b10010000) == 0b10010000) { //attenuation
        uint8_t atten = Data & 0b00001111;
        if (atten == 0b1111) { //full atten, off.
            ChannelMgr_States[ch] &= ~CHSTATE_KON;
        } else {
            ChannelMgr_States[ch] |= CHSTATE_KON | CHSTATE_PARAM;
        }
    }
    if ((Data & 0b10010000) == 0b10000000) { //frequency low bits
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    }
    if ((Data & 0b10000000) == 0) { //frequency high bits
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    }
    if ((Data & 0b11110000) == 0b11100000) { //noise params
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    }
}

void Driver_ResetChips() {
        Driver_PsgOut(0b10011111);
        Driver_PsgOut(0b10111111);
        Driver_PsgOut(0b11011111);
        Driver_PsgOut(0b11111111);
        Driver_SrBuf[SR_CONTROL] ^= SR_BIT_IC;
        Driver_Output();
        Driver_Sleep(1000);
        Driver_SrBuf[SR_CONTROL] |= SR_BIT_IC;
        Driver_Output();
        Driver_Sleep(1000);
}

void Driver_FmOut(uint8_t Port, uint8_t Register, uint8_t Value) {
    if (Port == 0) {
        Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A1; //clear A1
    } else if (Port == 1) {
        Driver_SrBuf[SR_CONTROL] |= SR_BIT_A1; //set A1
    }
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_A0; //clear A0
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_FM_CS; // /cs low
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; // /wr low
    Driver_SrBuf[SR_DATABUS] = Register;
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_A0; //set A0
    Driver_SrBuf[SR_CONTROL] &= ~SR_BIT_WR; // /wr low
    Driver_SrBuf[SR_DATABUS] = Value;
    Driver_Output();
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_WR; // /wr high
    Driver_SrBuf[SR_CONTROL] |= SR_BIT_FM_CS; // /cs high
    Driver_Output();
    Driver_Sleep(5);

    //channel led stuff
    if (Driver_NoLeds) return;
    //todo: clean this up, there's so much code duplication.
    if (Register >= 0xb0 && Register <= 0xb2) {
        uint8_t ch = (Port?3:0)+(Register-0xb0);
        Driver_FmAlgo[ch] = Value & 0b111;
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Port == 0 && Register == 0x28) { //KON
        uint8_t ch = Value & 0b111;
        if (ch >= 0b100) ch = 0b11 + (ch - 0b100);
        //uint8_t st = Value >> 4;
        uint8_t st = 0;
        switch (Driver_FmAlgo[ch]) {
            case 0:
            case 1:
            case 2:
            case 3:
                st = Value & 0b10000000;
                break;
            case 4:
                st = Value & 0b11000000;
                break;
            case 5:
            case 6:
                st = Value & 0b11100000;
                break;
            case 7:
                st = Value & 0b11110000;
                break;
            default: /*not possible*/
                break;
        }
        if (st) {
            ChannelMgr_States[ch] |= CHSTATE_KON;
        } else {
            ChannelMgr_States[ch] &= ~CHSTATE_KON;
        }
    } else if (Register >= 0xa0 && Register <= 0xae) { //frequency
        uint8_t ch = (Port?3:0) + min(2,Register-0xa0); //this is wacky, but it's to deal with ch3 special mode.
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register == 0x2a) { //dac value
        if (Value >= 0x7f) {
            ChannelMgr_PcmAccu += Value - 0x7f;
        } else {
            ChannelMgr_PcmAccu += 0x7f - Value;
        }
        ChannelMgr_PcmCount++;
    } else if (Register >= 0x30 && Register <= 0x3e) { //multiply, detune
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x40 && Register <= 0x4e) { //TL
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x50 && Register <= 0x5e) { //attack rate/scaling
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x60 && Register <= 0x6e) { //1st decay, am enable
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x70 && Register <= 0x7e) { //2nd decay
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x80 && Register <= 0x8e) { //release rate, sustain
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register >= 0x90 && Register <= 0x9e) { //SSG-EG
        uint8_t ch = (Port?3:0)+(Register%3);
        ChannelMgr_States[ch] |= CHSTATE_PARAM;
    } else if (Register == 0x2b) { //dac enable
        if (Value & 0x80) ChannelMgr_States[5] |= CHSTATE_DAC;
        else ChannelMgr_States[5] &= ~CHSTATE_DAC;
    }
}

void Driver_UpdateCh6Muting() {
    if (Driver_MitigateVgmTrim) {
        if (Driver_FirstWait) {
            Driver_FmOut(1, 0xb6, Driver_FmPans[5] & 0b00111111);
            return;
        }
    }
    uint8_t reg = Driver_FmPans[5] & (Driver_FmMask & (1<<(Driver_DacEn?6:5))?0b11111111:0b00111111);
    if (Driver_ForceMono && (reg & 0b11000000)) reg |= 0b11000000;
    Driver_FmOut(1, 0xb6, reg);
}

void Driver_UpdateMuting() {
    if (Driver_MitigateVgmTrim) {
        if (Driver_FirstWait) {
            //force everything off no matter what
            Driver_FmOut(0, 0xb4, Driver_FmPans[0] & 0b00111111);
            Driver_FmOut(0, 0xb5, Driver_FmPans[1] & 0b00111111);
            Driver_FmOut(0, 0xb6, Driver_FmPans[2] & 0b00111111);
            Driver_FmOut(1, 0xb4, Driver_FmPans[3] & 0b00111111);
            Driver_FmOut(1, 0xb5, Driver_FmPans[4] & 0b00111111);
            Driver_FmOut(1, 0xb6, Driver_FmPans[5] & 0b00111111);
            for (uint8_t i=0;i<4;i++) {
                Driver_PsgOut(0b10011111 | (i<<5));
            }
            return;
        }
    }
    for (uint8_t i=0;i<5;i++) {
        uint8_t mask = (Driver_FmMask & (1<<i))?0b11111111:0b00111111;
        uint8_t reg = Driver_FmPans[i] & mask;
        if (Driver_ForceMono && (reg & 0b11000000)) reg |= 0b11000000;
        Driver_FmOut((i<3)?0:1, 0xb4 + ((i<3)?i:(i-3)), reg);
    }
    Driver_UpdateCh6Muting();
    for (uint8_t i=0;i<4;i++) {
        uint8_t atten = 0;
        if (Driver_PsgMask & (1<<i)) {
            atten = Driver_PsgAttenuation[i];
        } else {
            atten = 0b10011111 | (i<<5);
        }
        Driver_PsgOut(atten);
    }
}

void Driver_SetFirstWait() {
    Driver_FirstWait = false;
    Driver_UpdateMuting();
    Driver_Cycle = 0;
    Driver_LastCc = Driver_Cc = xthal_get_ccount();
}

uint8_t Driver_SeqToSlot(uint32_t seq) {
    for (uint8_t i=0;i<DACSTREAM_PRE_COUNT;i++) {
        if (!DacStreamEntries[i].SlotFree && DacStreamEntries[i].Seq == seq) {
            return i;
        }
    }
    ESP_LOGE(TAG, "Dacstream sync error - failed to find entry for seq %d !!", seq);
    return 0xff;
}

uint8_t Driver_PsgFreqLow = 0; //used for sega psg fix
bool Driver_RunCommand(uint8_t CommandLength) { //run the next command in the queue. command length as a parameter just to avoid looking it up a second time
    uint8_t cmd[CommandLength]; //buffer for command + attached data
    
    //read command + data from the queue
    for (uint8_t i=0;i<CommandLength;i++) {
        cmd[i] = Ringbuf_Pop_Single(&Driver_CommandRingbuf);
    }

    if (cmd[0] == 0x50) { //SN76489
        //psg writes need to be intercepted to fix frequency register differences between TI PSG <-> SEGA VDP PSG
        //TODO: only do this for vgms where ver >= 1.51 and header 0x2b bit 0 is set
        //TODO: fix all the logic here - it's fucked up if fixpsgfreq = false
        if (Driver_FixPsgFrequency) {
            if ((cmd[1] & 0x80) == 0) { //ch 1~3 frequency high byte write
                if ((Driver_PsgFreqLow & 0b00001111) == 0) { //if low byte is all 0 for freq
                    if ((cmd[1] & 0b00111111) == 0) { //if high byte is all 0 for freq
                        Driver_PsgFreqLow |= 1; //set bit 0 of freq to 1
                    }
                }
                //write both registers now
                Driver_PsgOut(Driver_PsgFreqLow);
                Driver_PsgOut(cmd[1]);
            } else if ((cmd[1] & 0b10010000) == 0b10000000 && (cmd[1]&0b01100000)>>5 != 3) { //ch 1~3 frequency low byte write
                Driver_PsgFreqLow = cmd[1]; //just store the value for now, don't write anything until we get the high byte
            } else { //attenuation or noise ch control write
                if ((cmd[1] & 0b10010000) == 0b10010000) { //attenuation
                    uint8_t ch = (cmd[1]>>5)&0b00000011;
                    Driver_PsgAttenuation[ch] = cmd[1];
                    if (Driver_MitigateVgmTrim && Driver_FirstWait) cmd[1] |= 0b00001111; //if we haven't reached the first wait, force full attenuation
                    if ((Driver_PsgMask & (1<<ch)) == 0) {
                        cmd[1] |= 0b00001111;
                    }
                }
                Driver_PsgOut(cmd[1]);
            }
        } else { //not fixing psg frequency
            Driver_PsgOut(cmd[1]); //just write it normally
        }
    } else if (cmd[0] == 0x52) { //YM2612 port 0
        if (cmd[1] >= 0xb4 && cmd[1] <= 0xb6) { //pan, FMS, AMS
            Driver_FmPans[cmd[1]-0xb4] = cmd[2];
            if (Driver_MitigateVgmTrim && Driver_FirstWait) cmd[2] &= 0b00111111; //if we haven't reached the first wait, disable both L and R
            cmd[2] &= (Driver_FmMask & (1<<(cmd[1]-0xb4)))?0b11111111:0b00111111;
            if (Driver_ForceMono && (cmd[2] & 0b11000000)) cmd[2] |= 0b11000000;
        }
        if (cmd[1] == 0x2b && (cmd[2] & 0x80) != Driver_DacEn) {
            Driver_DacEn = cmd[2] & 0x80; //we shouldn't have to mask off the other bits but who knows what kind of crazy shit games do
            ESP_LOGD(TAG, "dac mode change %02x", Driver_DacEn);
            Driver_UpdateCh6Muting();
        }
        Driver_FmOut(0, cmd[1], cmd[2]);
    } else if (cmd[0] == 0x53) { //YM2612 port 1
        if (cmd[1] >= 0xb4 && cmd[1] <= 0xb6) { //pan, FMS, AMS
            Driver_FmPans[3+cmd[1]-0xb4] = cmd[2];
            if (cmd[1] == 0xb6) { //ch6, we need to check if it's in dac mode or not
                if (Driver_DacEn) {
                    cmd[2] &= (Driver_FmMask & (1<<6))?0b11111111:0b00111111;
                } else {
                    cmd[2] &= (Driver_FmMask & (1<<5))?0b11111111:0b00111111;
                }
            } else { //otherwise apply muting masks as normal
                cmd[2] &= (Driver_FmMask & (1<<(3+(cmd[1]-0xb4))))?0b11111111:0b00111111;
            }
            if (Driver_MitigateVgmTrim && Driver_FirstWait) cmd[2] &= 0b00111111; //if we haven't reached the first wait, disable both L and R. do this after the muting logic
            if (Driver_ForceMono && (cmd[2] & 0b11000000)) cmd[2] |= 0b11000000;
        }
        Driver_FmOut(1, cmd[1], cmd[2]);
    } else if (cmd[0] == 0x61) { //16bit wait
        Driver_NextSample += *(uint16_t*)&cmd[1];
        if (Driver_FirstWait && *(uint16_t*)&cmd[1] > 0) {
            Driver_SetFirstWait();
        }
    } else if (cmd[0] == 0x62) { //60Hz wait
        Driver_NextSample += 735;
        if (Driver_FirstWait) Driver_SetFirstWait();
    } else if (cmd[0] == 0x63) { //50Hz wait
        Driver_NextSample += 882;
        if (Driver_FirstWait) Driver_SetFirstWait();
    } else if ((cmd[0] & 0xf0) == 0x70) { //4bit wait
        Driver_NextSample += (cmd[0] & 0x0f) + 1;
        if (Driver_FirstWait) Driver_SetFirstWait();
    } else if ((cmd[0] & 0xf0) == 0x80) { //YM2612 DAC + wait
        uint8_t sample;
        //TODO check if queue is empty
        sample = Ringbuf_Pop_Single(&Driver_PcmRingbuf);
        Driver_FmOut(0, 0x2a, sample);
        Driver_NextSample += cmd[0] & 0x0f;
        if (Driver_FirstWait && (cmd[0] & 0x0f) > 0) {
            Driver_SetFirstWait();
        }
    } else if (cmd[0] == 0x93 || cmd[0] == 0x95) { //dac stream start
        DacStreamSeq++;
        if (DacStreamLastSeqPlayed > 0) {
            for (uint32_t s=DacStreamLastSeqPlayed;s<DacStreamSeq;s++) {
                uint8_t id = Driver_SeqToSlot(s);
                if (id != 0xff) {
                    DacStreamEntries[id].SlotFree = true;
                }
            }
        }
        uint8_t id;
        id = Driver_SeqToSlot(DacStreamSeq);
        if (id == 0xff) {
            ESP_LOGW(TAG, "DacStreamEntries under !!");
        } else {
            DacStreamId = id;
            DacStreamSampleRate = DacStreamEntries[DacStreamId].SampleRate;
            DacStreamPort = DacStreamEntries[DacStreamId].ChipPort;
            DacStreamCommand = DacStreamEntries[DacStreamId].ChipCommand;
            DacStreamSampleTime = xthal_get_ccount();
            DacStreamLastSeqPlayed = DacStreamSeq;
            DacStreamSamplesPlayed = 0;
            DacStreamLengthMode = DacStreamEntries[DacStreamId].LengthMode;
            DacStreamDataLength = DacStreamEntries[DacStreamId].DataLength;
            ESP_LOGD(TAG, "playing %d q size %d rate %d LM %d len %d", DacStreamSeq, uxQueueMessagesWaiting(DacStreamEntries[DacStreamId].Queue), DacStreamSampleRate, DacStreamLengthMode, DacStreamDataLength);
            DacStreamActive = true;
        }
    } else if (cmd[0] == 0x94) { //dac stream stop
        DacStreamActive = false;
    } else if (cmd[0] == 0x92) { //set sample rate
        if (DacStreamActive) {
            DacStreamSampleRate = *(uint32_t*)&cmd[2];
            ESP_LOGD(TAG, "Dacstream samplerate updated to %d", DacStreamSampleRate);
        } else {
            ESP_LOGD(TAG, "Not updating dacstream samplerate, not playing");
        }
    } else if (cmd[0] == 0x90 || cmd[0] == 0x91) { //dacstream commands we don't need to worry about here

    } else if (cmd[0] == 0x4f) { //gamegear psg stereo
        ESP_LOGW(TAG, "Game Gear PSG stereo not implemented !!");
    } else if (cmd[0] == 0xb2) {
        if (cmd[1] & 0b11000000) ESP_LOGW(TAG, "Unsupported 32x pwm reg write %d !!", cmd[1]>>4);
    } else if (cmd[0] == 0x66) { //end of music
        xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_RUNNING);
        xEventGroupSetBits(Driver_CommandEvents, DRIVER_EVENT_FINISHED);
        Driver_ResetChips();
        ESP_LOGI(TAG, "reached end of music");
    } else {
        ESP_LOGE(TAG, "driver unknown command %02x !!", cmd[0]);
        return false;
    }
    return true;
}

uint32_t Driver_BusyStart = 0;
//uint32_t Driver_BusyEnd = 0;

void Driver_Main() {
    //driver task. never pet watchdog or come up for air at all - nothing else is running on CPU1.
    ESP_LOGI(TAG, "Task start");
    while (1) {
        Driver_Cc = xthal_get_ccount();
        uint8_t commandeventbits = xEventGroupGetBits(Driver_CommandEvents);
        uint8_t queueeventbits = xEventGroupGetBits(Driver_QueueEvents);
        if (commandeventbits & DRIVER_EVENT_START_REQUEST) {
            //reset all internal vars
            Driver_Sample = 0;
            Driver_Cycle = 0;
            Driver_LastCc = Driver_Cc;
            Driver_NextSample = 0;
            DacStreamActive = false;
            DacStreamSeq = 0;
            memset(&Driver_FmAlgo[0], 0, sizeof(Driver_FmAlgo));
            Driver_DacEn = 0;
            Driver_PsgLastChannel = 0; //psg can't really be reset, so technically this is kinda wrong? but it's consistent.
            Driver_FirstWait = true;
            memset(&Driver_FmPans[0], 0b11000000, sizeof(Driver_FmPans));
            for (uint8_t i=0;i<4;i++) {
                Driver_PsgAttenuation[i] = 0b10011111 | (i<<5);
            }
            Driver_UpdateMuting();
            memset(&ChannelMgr_States[0], 0, 6+4);

            //update status flags
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_FINISHED);
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_START_REQUEST);
            xEventGroupSetBits(Driver_CommandEvents, DRIVER_EVENT_RUNNING);
            commandeventbits &= ~DRIVER_EVENT_START_REQUEST;
            commandeventbits |= DRIVER_EVENT_RUNNING;
        } else if (commandeventbits & DRIVER_EVENT_UPDATE_MUTING) {
            ESP_LOGI(TAG, "updating muting upon request");
            Driver_UpdateMuting();
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_UPDATE_MUTING);
            commandeventbits &= ~DRIVER_EVENT_UPDATE_MUTING;
        } else if (commandeventbits & DRIVER_EVENT_RESET_REQUEST) {
            Driver_ResetChips();
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_FINISHED);
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_RESET_REQUEST);
            xEventGroupSetBits(Driver_CommandEvents, DRIVER_EVENT_RESET_ACK);
            commandeventbits &= ~DRIVER_EVENT_RESET_REQUEST;
            commandeventbits |= DRIVER_EVENT_RESET_ACK;
        } else if (commandeventbits & DRIVER_EVENT_STOP_REQUEST) {
            Driver_PauseSample = Driver_Sample;
            Driver_NoLeds = true;
            Driver_FmOut(0, 0xb4, Driver_FmPans[0] & 0b00111111);
            Driver_FmOut(0, 0xb5, Driver_FmPans[1] & 0b00111111);
            Driver_FmOut(0, 0xb6, Driver_FmPans[2] & 0b00111111);
            Driver_FmOut(1, 0xb4, Driver_FmPans[3] & 0b00111111);
            Driver_FmOut(1, 0xb5, Driver_FmPans[4] & 0b00111111);
            Driver_FmOut(1, 0xb6, Driver_FmPans[5] & 0b00111111);
            for (uint8_t i=0;i<4;i++) {
                Driver_PsgOut(0b10011111 | (i<<5));
            }
            Driver_NoLeds = false;
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_FINISHED);
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_RUNNING);
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_STOP_REQUEST);
            commandeventbits &= ~(DRIVER_EVENT_RUNNING | DRIVER_EVENT_STOP_REQUEST);
        } else if (commandeventbits & DRIVER_EVENT_RESUME_REQUEST) {
            //todo: what if higher up resumes before stopping?
            Driver_NoLeds = true;
            Driver_UpdateMuting();
            Driver_NoLeds = false;
            Driver_Cc = Driver_LastCc = DacStreamSampleTime = xthal_get_ccount(); //dacstreams may end up being off by a sample or two upon resume - not going to worry about it
            Driver_Sample = Driver_PauseSample;
            xEventGroupSetBits(Driver_CommandEvents, DRIVER_EVENT_RUNNING);
            xEventGroupClearBits(Driver_CommandEvents, DRIVER_EVENT_RESUME_REQUEST);
            commandeventbits &= ~DRIVER_EVENT_RESUME_REQUEST;
            commandeventbits |= DRIVER_EVENT_RUNNING;
        } else if (commandeventbits & DRIVER_EVENT_RUNNING) {
            Driver_Cycle += (Driver_Cc - Driver_LastCc);
            Driver_LastCc = Driver_Cc;
            Driver_Sample = Driver_Cycle / DRIVER_CYCLES_PER_SAMPLE;
            
            //vgm stuff
            if (Driver_Sample >= Driver_NextSample) { //time to move on to the next sample
                Driver_BusyStart = xthal_get_ccount();
                uint32_t waiting = Driver_CommandRingbuf.Size - Ringbuf_Available(&Driver_CommandRingbuf);
                if (waiting > 0) { //is any data available?
                    //update half-empty event bit
                    if ((queueeventbits & DRIVER_EVENT_COMMAND_HALF) == 0 && waiting <= DRIVER_QUEUE_SIZE/2) {
                        xEventGroupSetBits(Driver_QueueEvents, DRIVER_EVENT_COMMAND_HALF);
                        queueeventbits |= DRIVER_EVENT_COMMAND_HALF;
                    } else if ((queueeventbits & DRIVER_EVENT_COMMAND_HALF) && waiting > DRIVER_QUEUE_SIZE/2) {
                        xEventGroupClearBits(Driver_QueueEvents, DRIVER_EVENT_COMMAND_HALF);
                        queueeventbits &= ~DRIVER_EVENT_COMMAND_HALF;
                    }

                    uint8_t peeked;
                    peeked = Ringbuf_Peek_Single(&Driver_CommandRingbuf); //peek at first command in the queue
                    uint8_t cmdlen = VgmCommandLength(peeked); //look up the length of this command + attached data
                    if (waiting >= cmdlen) { //if the entire command + data is in the queue
                        bool ret = Driver_RunCommand(cmdlen);
                        if (!ret) {
                            printf("ERR command run fail");
                            fflush(stdout);
                            /*xEventGroupSetBits(Driver_CommandEvents, DRIVER_EVENT_ERROR);
                            commandeventbits |= DRIVER_EVENT_ERROR;*/
                        }
                    } else { //not enough data in queue - underrun
                        xEventGroupSetBits(Driver_QueueEvents, DRIVER_EVENT_COMMAND_UNDERRUN);
                        queueeventbits |= DRIVER_EVENT_COMMAND_UNDERRUN;
                        printf("UNDER data\n");
                        fflush(stdout);
                    }
                } else { //no data at all in queue - underrun
                    xEventGroupSetBits(Driver_QueueEvents, DRIVER_EVENT_COMMAND_UNDERRUN);
                    queueeventbits |= DRIVER_EVENT_COMMAND_UNDERRUN;
                    printf("UNDER none\n");
                    fflush(stdout);
                }
                Driver_CpuUsageVgm += (xthal_get_ccount() - Driver_BusyStart);
            } else {
                //not time for next sample yet
                if (Driver_NextSample - Driver_Sample > 2000 && !DacStreamActive) vTaskDelay(2);
            }

            //dacstream stuff
            if (DacStreamActive) {
                //todo: gracefully handle the end of a stream
                //can't just go by bytes played because some play modes are based on time
                //decide whether those are worth implementing
                Driver_BusyStart = xthal_get_ccount();
                if (uxQueueMessagesWaiting(DacStreamEntries[DacStreamId].Queue)) {
                    if (xthal_get_ccount() - DacStreamSampleTime >= (DRIVER_CLOCK_RATE/DacStreamSampleRate)) {
                        uint8_t sample;
                        xQueueReceive(DacStreamEntries[DacStreamId].Queue, &sample, 0);
                        Driver_FmOut(DacStreamPort, DacStreamCommand, sample);
                        DacStreamSampleTime += (DRIVER_CLOCK_RATE/DacStreamSampleRate);
                        DacStreamSamplesPlayed++;
                        if (DacStreamSamplesPlayed == DacStreamDataLength && (DacStreamLengthMode == 0 || DacStreamLengthMode == 1 || DacStreamLengthMode == 3)) {
                            DacStreamActive = false;
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "DacStream sample queue under !! pos %d length %d", DacStreamSamplesPlayed, DacStreamDataLength);
                    //DacStreamActive = false;
                }
                Driver_CpuUsageDs += (xthal_get_ccount() - Driver_BusyStart);
            }
        } else {
            //not running
            vTaskDelay(2);
        }
    }
}