/**
 	 ________  _   __   _
	|_   __  |(_) [  | / |_
	  | |_ \_|__   | |`| |-'.---.  _ .--.  ,--.
	  |  _|  [  |  | | | | / /__\\[ `/'`\]`'_\ :
	 _| |_    | |  | | | |,| \__., | |    // | |,
	|_____|  [___][___]\__/ '.__.'[___]   \'-;__/
                                              (c)

	Copyright 2022 University of Alberta

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
 */
/**
 * @file run_filter.c
 * @author Andrew R. Rooney
 * @date 2022-03-26
 */

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <stdio.h>

#include "platform.h"
#include "xil_printf.h"
#include "xscugic.h"
#include "intc/intc.h"
#include "dma/dma.h"
#include "xparameters.h"
#include "xaxidma.h"
#include "rnnoise/rnnoise.h"
#include "xtime_l.h"
#include "sleep.h"

//#define DO_TIMING // define to run timer on RNN.

#define FRAME_SIZE 480
#define RECORD_LEN (480*4)

/* Forward declarations */
static void initTask(void* param);
void rxTask(void* param);
void txTask(void* param);
void processTask(void* param);

/* Globals */
XTime start_g, end_g; // for time measurements
static QueueHandle_t Audio_Queue, Filtered_Audio_Queue;
extern QueueHandle_t Incoming_Blocks, Outgoing_Blocks; // data in/out from DMA
static XScuGic sIntc;
static XAxiDma sAxiDma;

/* Interrupt vector table */
const ivt_t ivt[] = {
	{XPAR_FABRIC_AUDIO_25MHZ_AXI_DMA_0_S2MM_INTROUT_INTR, (Xil_ExceptionHandler)fnS2MMInterruptHandler, &sAxiDma},
	{XPAR_FABRIC_AUDIO_25MHZ_AXI_DMA_0_MM2S_INTROUT_INTR, (Xil_ExceptionHandler)fnMM2SInterruptHandler, &sAxiDma},
};

// Entry point:
int main()
{
	xTaskCreate( initTask,
				 ( const char * ) "Initialization",
				 2048,
				 NULL,
				 tskIDLE_PRIORITY + 4,
				 NULL );

	vTaskStartScheduler();
	for(;;); // Should not be here.
    cleanup_platform();
    return 0;
}

/**
 * @brief Initialize the platform, and spawn processing tasks.
 * 
 * @param param Pointer to the task's parameter (unused).
 */
static void initTask(void* param)
{
	for (;;)
	{
	    init_platform();
	    int Status;

		Status = fnInitInterruptController(&sIntc);
		if(Status != XST_SUCCESS) {
			xil_printf("Error initializing interrupts");
			return;
		}

		fnEnableInterrupts(&sIntc, &ivt[0], sizeof(ivt)/sizeof(ivt[0]));

		Audio_Queue = xQueueCreate(5, RECORD_LEN*2*sizeof(float));

		Filtered_Audio_Queue = xQueueCreate(5, RECORD_LEN*2*sizeof(uint32_t));

		if (Audio_Queue == NULL || Filtered_Audio_Queue == NULL) return;

		xTaskCreate( txTask,
					 ( const char * ) "Transmit Samples",
					 8192,
					 NULL,
					 tskIDLE_PRIORITY + 2,
					 NULL );

		xTaskCreate( rxTask,
					 ( const char * ) "Receive Samples",
					 8192,
					 NULL,
					 tskIDLE_PRIORITY + 2,
					 NULL );

		xTaskCreate( processTask,
					 ( const char * ) "Receive Samples",
					 20000,
					 NULL,
					 tskIDLE_PRIORITY + 2,
					 NULL );

		Status = fnConfigDma(&sAxiDma);
		if(Status != XST_SUCCESS) {
			xil_printf("DMA configuration ERROR");
			return;
		}

		vTaskDelete(NULL); // a one-shot task
	}
}

/**
 * @brief Receive samples from the DMA and place them in a queue.
 * 
 * @param param Pointer to the task's parameter (unused).
 */
void rxTask(void* param)
{
	int64_t sample;
	float recording_float[RECORD_LEN*2];
	for (;;)
	{
		for (int i = 0; i < RECORD_LEN; i++)
		{
			while (xQueueReceive(Incoming_Blocks, &sample, pdMS_TO_TICKS(10)) != pdPASS);
			// Shift the sample to the right by 8 bits to convert
			// from 24-bit to 16-bit audio data. Then convert to float.
			int32_t tmp = (int32_t) ((sample & 0xFFFFFFFF00000000) >> 32);
			recording_float[2*i] = (float) ((short) ((tmp >> 8)));
			tmp = (int32_t) (sample & 0x00000000FFFFFFFF);
			recording_float[2*i+1] = (float) ((short) (tmp >> 8));
		}

		// Enqueue the recording for processing.
		xQueueSendToBack(Audio_Queue, recording_float, pdMS_TO_TICKS(5));
		taskYIELD();
	}
}

/**
 * @brief Receive recordings from the queue and process them in the
 * recursive neural network (RNN).
 * 
 * @param param Pointer to the task's parameter (unused).
 */
void processTask(void* param)
{
	static float frame[RECORD_LEN*2];
	static int32_t record[RECORD_LEN*2];
	DenoiseState *st = NULL;
	st = rnnoise_create(NULL);

	for (;;)
	{

		if (xQueueReceive(Audio_Queue, frame, portMAX_DELAY) != pdPASS) continue;

		for (int i = 0; i + FRAME_SIZE <= RECORD_LEN*2; i += FRAME_SIZE)
		{

#ifdef DO_TIMING
			XTime_GetTime(&start_g);
			char str[40];
#endif
			rnnoise_process_frame(st, frame + i, frame + i);
#ifdef DO_TIMING
			XTime_GetTime(&end_g);
			const double elapsed = (double) (((double)((u64) end_g- (u64) start_g))/COUNTS_PER_SECOND);
			sprintf(str, "end-start: %lu, elapsed[s]: %.7f\r\n", (u64) ((u64) end_g - (u64) start_g), elapsed);
			xil_printf(str);
#endif
		}

		for (int i = 0; i < RECORD_LEN*2; i++)
		{
			int32_t tmp = (int32_t) frame[i];
			record[i] = (int32_t) (((tmp  << 8)));
		}

		xQueueSendToBack(Filtered_Audio_Queue, record, pdMS_TO_TICKS(10));
		taskYIELD();
	}
}

/**
 * @brief Transmit samples from the queue to the DMA.
 * 
 * @param param Pointer to the task's parameter (unused).
 */
void txTask(void* param)
{
	static int32_t record[RECORD_LEN*2];
	unsigned int numWaiting = 0;
	do
	{
		// Fill up the data pipeline before transmitting...
		numWaiting = uxQueueMessagesWaiting(Filtered_Audio_Queue);
		vTaskDelay(pdMS_TO_TICKS(1));
	} while (numWaiting < 1);

	for (;;)
	{
		if (xQueueReceive(Filtered_Audio_Queue, record, portMAX_DELAY) != pdPASS) continue;
		sendBlock(&sAxiDma, (int64_t*) record, RECORD_LEN);
		taskYIELD();
	}
}
