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
 * @file dma.c
 * @author Andrew R. Rooney
 * @date 2022-03-26
 */

#include <FreeRTOS.h>
#include <semphr.h>
#include "dma.h"
#include "xtime_l.h"
#include "xil_printf.h"
#include "stdio.h"

/* Globals */
extern XAxiDma_Config *pCfgPtr;
QueueHandle_t Incoming_Blocks, Outgoing_Blocks;
static TaskHandle_t GetBlocksTask, SendBlocksTask;
extern XTime start_g, end_g;
extern char str[50];
static int first_start = 1, first_end = 1;
/**
 * @brief Handles the DMA interrupt from slave to memory master.
 * 
 * @param Callback DMA instance pointer.
 */
void fnS2MMInterruptHandler (void *Callback)
{
	u32 IrqStatus;
	int TimeOut;
	XAxiDma *AxiDmaInst = (XAxiDma*) Callback;
	BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
	//Read all the pending DMA interrupts
	IrqStatus = XAxiDma_IntrGetIrq(AxiDmaInst, XAXIDMA_DEVICE_TO_DMA);

	//Acknowledge pending interrupts
	XAxiDma_IntrAckIrq(AxiDmaInst, IrqStatus, XAXIDMA_DEVICE_TO_DMA);

	//If there are no interrupts we exit the Handler
	if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK))
	{
		return;
	}

	// If error interrupt is asserted, raise error flag, reset the
	// hardware to recover from the error, and return with no further
	// processing.
	if (IrqStatus & XAXIDMA_IRQ_ERROR_MASK)
	{
		XAxiDma_Reset(AxiDmaInst);
		TimeOut = 1000;
		while (TimeOut)
		{
			if(XAxiDma_ResetIsDone(AxiDmaInst))
			{
				break;
			}
			TimeOut -= 1;
		}
		return;
	}

	if ((IrqStatus & XAXIDMA_IRQ_IOC_MASK))
	{
		xTaskNotifyFromISR(GetBlocksTask, 0, eSetValueWithOverwrite, &pxHigherPriorityTaskWoken);
		portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
	}
}

/**
 * @brief Blocks the calling task until the DMA has finished receiving all the blocks.
 * 
 * @return int Notification value.
 */
int blockOnS2MMEvent()
{
	uint32_t index = 0;
    xTaskNotifyWait(0x00,
				0xFFFFFFFF,
				&index,
				portMAX_DELAY );
	return index;
}


/**
 * @brief Initialize RX transfers, and enqueue the received DMA blocks.
 * 
 * @param AxiDma Task parameter holding the DMA instance.
 */
void getBlockTask(void *AxiDma)
{
	// initialize transfer of 64 bytes (one L, one R sample) - continuous!
	static int64_t sample[2];
	Xil_DCacheInvalidateRange((u64) sample, 2*sizeof(int64_t));
	XAxiDma_SimpleTransfer( (XAxiDma*) AxiDma, (u64) sample, 2*sizeof(int64_t), XAXIDMA_DEVICE_TO_DMA);
	// once done, get address of range, and copy to a queue
	for (;;)
	{
		blockOnS2MMEvent();
//		if (first_start) XTime_GetTime(&start_g);
//		first_start = 0;

		// Give to queue at index RxSectionDoneIdx
		Xil_DCacheInvalidateRange((u64) sample, 2*sizeof(int64_t));
		Xil_DCacheFlushLine((u64) sample);
		while (xQueueSendToBack(Incoming_Blocks, sample, portMAX_DELAY) != pdPASS);
		Xil_DCacheInvalidateRange((u64) sample, 2*sizeof(int64_t));
		Xil_DCacheFlushLine((u64) sample);
		XAxiDma_SimpleTransfer( (XAxiDma*) AxiDma, (u64) sample, 2*sizeof(int64_t), XAXIDMA_DEVICE_TO_DMA);
		Xil_DCacheFlushLine((u64) sample);
		taskYIELD();
	}
}

/**
 * @brief Handles the DMA interrupt from memory master to slave.
 * 
 * @param Callback DMA instance pointer.
 */
void fnMM2SInterruptHandler (void *Callback)
{

	u32 IrqStatus;
	int TimeOut;
	XAxiDma *AxiDmaInst = (XAxiDma *)Callback;
	BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
	//Read all the pending DMA interrupts
	IrqStatus = XAxiDma_IntrGetIrq(AxiDmaInst, XAXIDMA_DMA_TO_DEVICE);
	//Acknowledge pending interrupts
	XAxiDma_IntrAckIrq(AxiDmaInst, IrqStatus, XAXIDMA_DMA_TO_DEVICE);
	//If there are no interrupts we exit the Handler
	if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK))
	{
		return;
	}

	// If error interrupt is asserted, raise error flag, reset the
	// hardware to recover from the error, and return with no further
	// processing.
	if (IrqStatus & XAXIDMA_IRQ_ERROR_MASK){
		XAxiDma_Reset(AxiDmaInst);
		TimeOut = 1000;
		while (TimeOut)
		{
			if(XAxiDma_ResetIsDone(AxiDmaInst))
			{
				break;
			}
			TimeOut -= 1;
		}
		return;
	}
	if ((IrqStatus & XAXIDMA_IRQ_IOC_MASK))
	{
		xTaskNotifyFromISR(SendBlocksTask, 0, eSetValueWithOverwrite, &pxHigherPriorityTaskWoken);
	}
	portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
}

/**
 * @brief Blocks the calling task until the DMA has finished sending all the blocks.
 * 
 * @return int Notification value.
 */
int blockOnMM2SEvent()
{
	uint32_t index = 0;
    xTaskNotifyWait(0x00,
				0xFFFFFFFF,
				&index,
				portMAX_DELAY );
	return index;
}

/**
 * @brief Send blocks to the DMA. Will enqueue the given block the the outgoing queue to
 * be sent to the audio device.
 * 
 * @param AxiDma Pointer to the DMA instance.
 * @param block Pointer to the block to be sent.
 * @param len Length of the block.
 */
void sendBlock(XAxiDma *AxiDma, int64_t *block, int len)
{
	for (int i = 0; i < len; i++)
	{
		xQueueSendToBack(Outgoing_Blocks, &block[i], pdMS_TO_TICKS(10));
	}
}


/**
 * @brief Task that sends blocks from the outgoing data queue to the DMA.
 * 
 * @param axi Task parameter holding the DMA instance.
 */
void sendTask(void* axi)
{
	static int64_t out[2];
	for (;;)
	{
		xQueueReceive(Outgoing_Blocks, out, portMAX_DELAY);

//		if (first_end)
//		{
//			XTime_GetTime(&end_g);
//			const double elapsed = (double) (((double)((u64) end_g- (u64) start_g))/COUNTS_PER_SECOND);
//			sprintf(str, "end-start: %lu, elapsed[s]: %.7f\r\n", (u64) ((u64) end_g - (u64) start_g), elapsed);
//			xil_printf(str);
//			first_end = 0;
//		}

		Xil_DCacheFlush();
		XAxiDma_SimpleTransfer( (XAxiDma*) axi, (u64) out, 2*sizeof(u64), XAXIDMA_DMA_TO_DEVICE);
		blockOnMM2SEvent();
	}
}

/**
 * @brief Configure the given DMA instance to be used for the audio device.
 * 
 * @param AxiDma Pointer to the DMA instance.
 * @return XStatus Status of the DMA configuration (XST_SUCCESS if successful).
 */
XStatus fnConfigDma(XAxiDma *AxiDma)
{
	int Status;
	XAxiDma_Config *pCfgPtr;

	//Make sure the DMA hardware is present in the project
	pCfgPtr = XAxiDma_LookupConfig(XPAR_AXIDMA_0_DEVICE_ID);
	if (!pCfgPtr)
	{

		return XST_FAILURE;
	}

	//Initialize DMA
	Status = XAxiDma_CfgInitialize(AxiDma, pCfgPtr);
	if (Status != XST_SUCCESS)
	{

		return XST_FAILURE;
	}

	//Ensures that the Scatter Gather mode is not active
	if(XAxiDma_HasSg(AxiDma))
	{

		return XST_FAILURE;
	}

	//Disable all the DMA related Interrupts
	XAxiDma_IntrDisable(AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
	XAxiDma_IntrDisable(AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

	//Enable all the DMA Interrupts
	XAxiDma_IntrEnable(AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
	XAxiDma_IntrEnable(AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

	Incoming_Blocks = xQueueCreate(2400, 2*sizeof(int64_t));
	Outgoing_Blocks = xQueueCreate(2400, 2*sizeof(int64_t));

	if (xTaskCreate( getBlockTask,
				 ( const char * ) "Receive block",
				 512,
				 (void*) AxiDma,
				 tskIDLE_PRIORITY + 2,
				 &GetBlocksTask ) != pdPASS) return XST_FAILURE;

	if (xTaskCreate( sendTask,
				 ( const char * ) "send block",
				 512,
				 (void*) AxiDma,
				 tskIDLE_PRIORITY + 2,
				 &SendBlocksTask ) != pdPASS) return XST_FAILURE;

	return XST_SUCCESS;
}
