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
 * @file dma.h
 * @author Andrew R. Rooney
 * @date 2022-03-26
 */

#ifndef DMA_H_
#define DMA_H_

#include <FreeRTOS.h>
#include <queue.h>
#include "xparameters.h"
#include "xaxidma.h"

void fnS2MMInterruptHandler (void *Callback);

void fnMM2SInterruptHandler (void *Callback);

XStatus fnConfigDma(XAxiDma *AxiDma);

void sendBlock(XAxiDma *AxiDma, int64_t *block, int len);

#endif /* DMA_H_ */
