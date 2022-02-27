//****************************************************************************
// Copyright 2021 Richard Hulme
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Demo application to test the flashloader.
// Listens on the default UART for an Intel hex file containing a new
// application.  This is stored in flash and the system is rebooted into the
// flashloader which overwrites the existing application with the new image
// and boots into it.
// Because the flashloader is not overwriting itself, it is power-fail safe.
//
// This code is for demonstration purposes.  There is not very much
// error-checking and attempts have been made to keep the final code size
// small (e.g. not using printf)

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "flashloader.h"

#ifndef PICO_DEFAULT_LED_PIN
    #error This example needs a board with an LED
#endif

#ifndef LED_DELAY_MS
    #error LED_DELAY_MS must be defined!
#endif

#define STRINGIFY(x) #x
#define TO_TEXT(x) STRINGIFY(x)

// Intel HEX record
typedef struct
{
    uint8_t  count;
    uint16_t addr;
    uint8_t  type;
    uint8_t  data[256];
}tRecord;

// Intel HEX record types
static const uint8_t TYPE_DATA      = 0x00;
static const uint8_t TYPE_EOF       = 0x01;
static const uint8_t TYPE_EXTSEG    = 0x02;
static const uint8_t TYPE_STARTSEG  = 0x03;
static const uint8_t TYPE_EXTLIN    = 0x04;
static const uint8_t TYPE_STARTLIN  = 0x05;

// Offset within flash of the new app image to be flashed by the flashloader
static const uint32_t FLASH_IMAGE_OFFSET = 128 * 1024;

// Buffer to hold the incoming data before flashing
static union
{
    tFlashHeader header;
    uint8_t      buffer[sizeof(tFlashHeader) + 65536];
} flashbuf;

//****************************************************************************
bool repeating_timer_callback(struct repeating_timer *t)
{
    (void)t;
    gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);
    return true;
}

//****************************************************************************
// Simple CRC32 (no reflection, no final XOR) implementation.
// This can be done with a lookup table or using the DMA sniffer too.
uint32_t crc32(const uint8_t *data, uint32_t len, uint32_t crc)
{
    while(len--)
    {
        crc ^= (*data++ << 24);

        for(int bit = 0; bit < 8; bit++)
        {
            if(crc & (1L << 31))
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc = (crc << 1);
        }
    }
    return crc;
}

//****************************************************************************
// Converts an ASCII hex character into its binary representation.
// The existing value is shifted across one nibble before the new value is
// stored in the lower nibble.
// Returns non-zero if the character could be converted
int hex2nibble(char c, uint8_t* value)
{
    int success = 0;

    if(c >= '0' && c <= '9')
    {
        *value <<= 4;
        *value |= (uint8_t)(c - '0');
        success = 1;
    }
    else
    {
        c |= 32;
        if(c >= 'a' && c <= 'z')
        {
            *value <<= 4;
            *value |= (uint8_t)(c - 'a') + 10;
            success = 1;
        }
    }

    return success;
}

//****************************************************************************
// Converts two ASCII hex characters to an 8-bit binary value.
// Returns non-zero if valid hex characters were found
int parseHex(const char* str, uint8_t* value)
{
    int success;

    *value = 0;
    success = hex2nibble(*str++, value) && hex2nibble(*str, value);

    return success;
}

//****************************************************************************
// Converts an Intel hex record in text form to a binary representation.
// Returns non-zero if the text could be parsed successfully
int processRecord(const char* line, tRecord* record)
{
    int     success = 0;
    int     offset = 0;
    uint8_t value;
    uint8_t data[256 + 5]; // Max payload 256 bytes plus 5 for fields
    uint8_t checksum = 0;

    while(*line && (*line != ':'))
        line++;

    if(*line++ == ':')
    {
        while(parseHex(line, &value) && (offset < sizeof(data)))
        {
            data[offset++] = value;
            checksum += value;
            line += 2;
        }
    }

    // Checksum is two's-complement of the sum of the previous bytes so
    // final checksum should be zero if everything was OK.
    if((offset > 0) && (checksum == 0))
    {
        record->count = data[0];
        record->addr  = data[2] | (data[1] << 8);
        record->type  = data[3];
        memcpy(record->data, &data[4], data[0]);
        success = 1;
    }

    return success;
}

//****************************************************************************
// Store the given image in flash then reboot into the flashloader to replace
// the current application with the new image.
void flashImage(tFlashHeader* header, uint32_t length)
{
    // Calculate length of header plus length of data
    uint32_t totalLength = sizeof(tFlashHeader) + length;

    // Round erase length up to next 4096 byte boundary
    uint32_t eraseLength = (totalLength + 4095) & 0xfffff000;
    uint32_t status;

    header->magic1 = FLASH_MAGIC1;
    header->magic2 = FLASH_MAGIC2;
    header->length = length;
    header->crc32  = crc32(header->data, length, 0xffffffff);

    uart_puts(PICO_DEFAULT_UART_INSTANCE, "Storing new image in flash and then rebooting\r\n");

    status = save_and_disable_interrupts();

    flash_range_erase(FLASH_IMAGE_OFFSET, eraseLength);
    flash_range_program(FLASH_IMAGE_OFFSET, (uint8_t*)header, totalLength);

    restore_interrupts(status);
    uart_puts(PICO_DEFAULT_UART_INSTANCE, "Rebooting into flashloader in 1 second\r\n");

    // Set up watchdog scratch registers so that the flashloader knows
    // what to do after the reset
    watchdog_hw->scratch[0] = FLASH_MAGIC1;
    watchdog_hw->scratch[1] = XIP_BASE + FLASH_IMAGE_OFFSET;
    watchdog_reboot(0x00000000, 0x00000000, 1000);

    // Wait for the reset
    while(true)
        tight_loop_contents();
}

//****************************************************************************
// Reads a line of text from the standard UART into the given buffer and
// returns when a line-feed or carriage-return is detected.
char* getLine(char* buffer)
{
    char c;
    char* ptr = buffer;

    do
    {
        c = uart_getc(PICO_DEFAULT_UART_INSTANCE);

        if((c != '\n') && (c != '\r'))
            *ptr++ = c;
        else
            *ptr++ = 0;
    }while((c != '\n') && (c != '\r'));

    return buffer;
}


//****************************************************************************
// Reads an Intel hex file from the standard UART, stores it in flash then
// triggers the flashloader to overwrite the existing application with the
// new image.
void readIntelHex()
{
    uint32_t      offset = 0;
    char          line[1024];
    uint32_t      count = 0;

    while (true)
    {
        tRecord rec;

        if(processRecord(getLine(line), &rec))
        {
            switch(rec.type)
            {
                case TYPE_DATA:
                    memcpy(&flashbuf.header.data[offset], rec.data, rec.count);
                    offset += rec.count;
                    offset %= 65536;
                    if((offset % 1024) == 0)
                        uart_puts(PICO_DEFAULT_UART_INSTANCE, "Received block\r\n");
                    break;

                case TYPE_EOF:
                    flashImage(&flashbuf.header, offset);
                    break;

                case TYPE_EXTSEG:
                case TYPE_STARTSEG:
                case TYPE_STARTLIN:
                    // Ignore these types.  They aren't important for this demo
                    break;

                case TYPE_EXTLIN:
                    // Move to the start of the data buffer
                    offset = 0;
                    break;

                default:
                    break;
            }
            count++;
        }
    }
}


//****************************************************************************
// Entry point - start flashing the on-board LED and wait for a new
// application image.
int main()
{
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);

    uart_init(PICO_DEFAULT_UART_INSTANCE, 115200);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    struct repeating_timer timer;
    add_repeating_timer_ms(LED_DELAY_MS, repeating_timer_callback, NULL, &timer);

    uart_puts(PICO_DEFAULT_UART_INSTANCE, "Flashing LED every " TO_TEXT(LED_DELAY_MS) " milliseconds\r\n");

    readIntelHex();

    return 0;
}
