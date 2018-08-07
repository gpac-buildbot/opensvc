/*****************************************************************************
*
*  Open SVC Decoder developped in IETR image lab
*
*
*
*              Médéric BLESTEL <mblestel@insa-rennes.Fr>
*              Mickael RAULET <mraulet@insa-rennes.Fr>
*              http://www.ietr.org/
*
*
*
*
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*
*
* $Id$
*
**************************************************************************/

#include <string.h>
#include <stdlib.h>

//AVC Files
#include "type.h"
#include "display_management.h"

//SVC FIles
#include "symbol.h"
#include "svc_type.h"
#include "set_layer.h"
#include "SVCDisplay.h"
#include "ErrorDetection.h"



/**
This function flushes the stored memory address for the unused frame in the AU.
*/
void RemoveAuMemory(MMO* mmo){
	int i;

	for( i = 0; i < mmo -> AuMemory; i++){
		mmo -> FreeMemoryAddress[mmo -> NumFreeAddress] = mmo -> AuMemoryAddress[i];
		mmo -> AuMemoryAddress[i] = -1;
		mmo -> NumFreeAddress ++;
	}
	mmo -> AuMemory = 0;
}


/**
This functions tries to find why the current frame is not from the wanted DqId.
First, a change of layer has been asked, so the display is forced.
Second, the DqId asked is not anymore present into the stream. So we display the top layer.
*/
int CheckLayerToDisplay(NAL *Nal, MMO *Mmo, POC_PARAMS *CurrFrame)
{
	if(Nal -> FlushRenderer){
		//Layer has been changed
		//so we change the SameResolution variable
		return 1;
	}else{
		int j, SameResolution = 0, MultiResolution = 0, HighResolution;
		int DelayedPic = Mmo -> nb_img_display - Mmo -> num_free_poc_address;
		short LayerId[NUM_LAYERS];

		//Initialize to zero
		for(j = 0; j < NUM_LAYERS; j++){
			LayerId[j] = 0;
		}

		//Search for a frame from the right layer
		for(j = 0; j < DelayedPic && !SameResolution; j++){
			POC_PARAMS *Frame = &Mmo -> poc_params[j];
			//Take into account the frame only if it's not a ghost picture.
			if(!Frame -> is_ghost){
				LayerId[Frame -> LayerId]++;
			}
		}

		//Look if frames from different layers are present into the video renderer
		for(j = 0; j < NUM_LAYERS; j++){
			if(LayerId[j]){
				MultiResolution++;
				HighResolution = j;
			}
		}

		//In case of different layers into the renderer, we display only the hightest layer.
		if(MultiResolution > 1){
			if((Nal -> LayerIdTable[CurrFrame -> LayerId] >> 4) == (Nal -> LayerIdTable[HighResolution] >> 4)){
				return 1;
			}else {
				return 0;
			}
		}
		return !CurrFrame -> is_ghost;
	}
}

/**
This function searches into the frame buffer a frame with a specify POC.
*/
int SearchSamePOC(POC_PARAMS *Frame, LIST_MMO *Current_pic, NAL *Nal, int LayerNumber){

	int i;
	int Pos = -1;

	for ( i = 0; i < NUM_POC_PARAMS; i++){
		POC_PARAMS *CurrFrame = &Frame[NUM_POC_PARAMS - 1 - i];
		if (CurrFrame -> Picture -> poc == Current_pic -> poc){
			//A picture of a different layer exits
			//We just remember its position
			if (CurrFrame -> LayerId < LayerNumber){
				Pos = NUM_POC_PARAMS - i - 1;
			}
			if (CurrFrame -> Picture -> MemoryAddress == Current_pic -> MemoryAddress){
				int SameResolution = ((Nal -> LayerIdTable[CurrFrame -> LayerId] >> 4) == (Nal -> DqIdToDisplay >> 4));
				if(SameResolution){
					//SNR picture, same memory adresss. do nothing.
					CurrFrame -> LayerId = LayerNumber;
					Pos = -2;
				}
			}
			break;
		}
	}

	return Pos;
}



/**
Manage new pic (store, discard) according to the decoded layer.
*/
void ManageNewPic(NAL *Nal, MMO *Mmo, LIST_MMO *Current_pic, SPS *Sps, int layer_number, int WantedId)
{

	int Position = SearchSamePOC(Mmo -> poc_params, Current_pic, Nal, layer_number);

	if (Position >= 0){
		//We have to remove the wrong picture
		if (Nal -> DqId > WantedId || (Nal -> DiscardableFlag && Nal -> DqId != Nal -> DqIdToDisplay)){
			//The current picture is not the one to keep
			//We don't want to display this layer.
			//Just try to free the memory address
			Mmo -> FreeMemoryAddress[Mmo -> NumFreeAddress] = Current_pic -> MemoryAddress;
			Mmo -> NumFreeAddress++;
		}else {
			POC_PARAMS *Frame = &Mmo -> poc_params[Position];
			LAYER_MMO *LayerMmo = &Mmo -> LayerMMO [Frame -> LayerId]; 
			int FrameDependencyId = Nal -> LayerIdTable [Frame -> LayerId] >> 4;
			//Remove adress of the unused picture if necessary
			if ((FrameDependencyId == (Nal -> DqIdToDisplay >> 4)) && Nal -> UseRefBasePicFlag){			
				//Base layer reference addresses can be used for current layer, we have to check if its address is still used
				CurrentDisplay(Mmo, LayerMmo, Frame -> Picture, &Frame -> Picture -> MemoryAddress);
			}else {
				//Base layer reference picture are not used, so we can free there adresses
				Mmo -> AuMemoryAddress[Mmo -> AuMemory] = Frame -> Picture -> MemoryAddress;
				Mmo -> AuMemory++;  
			}

			Frame -> Picture -> displayed = 1;
			Mmo -> num_free_poc_address ++;

			StorePicture(&Mmo -> poc_params[Mmo -> nb_img_display - Mmo -> num_free_poc_address], Current_pic, Sps, layer_number);
			Mmo -> num_free_poc_address --;
		}
	} else if (Position == -1){
		//Test if there is at least one free memory.
		//If not, we find one
		if(!Mmo -> num_free_poc_address){
			RemoveDisplayError(Mmo, Nal);
		}

		StorePicture(&Mmo -> poc_params[Mmo -> nb_img_display - Mmo -> num_free_poc_address], Current_pic, Sps, layer_number);
		Mmo -> num_free_poc_address --;
	}
	else {
		int i;
		for ( i = 0; i < NUM_POC_PARAMS; i++){
			POC_PARAMS *CurrFrame = &Mmo -> poc_params[i];
			if (CurrFrame -> Picture -> poc == Current_pic -> poc){
				CurrFrame -> LayerId = layer_number;
			}
			break;
		}	
	}

#ifdef ERROR_DETECTION
	//Display management problem
	//No place in the renderer buffer and this frame has not base layer.
	//POC of an enhancement layer should be wrong
	if(Mmo -> num_free_poc_address == -1 && Position == -1){
		//printf("Missing poc params place 2\n");
		RemoveDisplayError(Mmo, Nal);
	}
#endif


	if (Nal -> DqIdMax == Nal -> DqId){
		RemoveAuMemory(Mmo);
		//Save last DqIdMax to know in the next NAL
		Nal -> LastDqIdMax = Nal -> DqIdMax;
	}
}


/**
Search into the stored frames which one has to be displayed.
*/
void SearchFrameToDisplay(NAL *Nal, MMO *Mmo, int *x_size, int *y_size, int *Crop, int *ImgToDisplay, int *address_pic)
{
	int BestPos, j;

	//Find the next frame to display
	POC_PARAMS *Frame = FindBestFrame(Mmo, Mmo -> poc_params, &BestPos, MIN_POC);

	//Check into the video renderer if the selected frame is displayable (right layer)
	int DisplayForced = CheckLayerToDisplay(Nal, Mmo, Frame);


	if (!Frame -> is_ghost && DisplayForced){
		//Try to know if we can release the memory address 
		int LayerNumber = GetLayerNumber(Nal, Nal -> DqIdToDisplay);
		LAYER_MMO *LayerMmo = &Mmo -> LayerMMO [LayerNumber];
		Mmo -> num_free_poc_address ++;
		Mmo -> LayerMMO [0] . num_decoded_frame++;
		CurrentDisplay(Mmo, LayerMmo, Frame -> Picture, address_pic);				
//		printf("\t display :%d %d %d %d\n", Mmo -> LayerMMO [0] . num_decoded_frame, Frame -> Picture -> poc, Frame -> x_size, Frame -> y_size);

		*ImgToDisplay = 1;
		*x_size = Frame -> x_size;
		*y_size = Frame -> y_size;
		*Crop = Frame -> Crop;

		if(Nal -> FlushRenderer){
			//Reset Flush renderer variable to indicate that the change of layer has been taken into account
			Nal -> FlushRenderer = 0;
		}

		memmove(&Mmo -> poc_params[BestPos], &Mmo -> poc_params[BestPos + 1], (Mmo -> nb_img_display - Mmo -> num_free_poc_address - BestPos + 1) * sizeof(POC_PARAMS));
	}else{
		//Ghost picture on a upper layer
		int NewPos;
		DisplayForced = 0;

		Mmo -> LayerMMO [0] . num_decoded_frame++;
		Mmo -> num_free_poc_address ++;

		//Release memory address.
		Mmo -> FreeMemoryAddress [Mmo -> NumFreeAddress] = Frame -> Picture -> MemoryAddress;
		Mmo -> NumFreeAddress++; 

		//Find the next picture of the layer to display, or of the hightest layer
		for(j = 0; j < Mmo -> nb_img_display - Mmo -> num_free_poc_address && !DisplayForced; j++){
			Frame = FindBestFrame(Mmo, Mmo -> poc_params, &NewPos, Frame -> Picture -> poc);
			DisplayForced = CheckLayerToDisplay(Nal, Mmo, Frame);
		}

		if(DisplayForced){
			*address_pic = Frame -> Picture -> MemoryAddress;
			*x_size = Frame -> x_size;
			*y_size = Frame -> y_size;
			*Crop = Frame -> Crop;
			*ImgToDisplay = 2;
		//	printf("\t display: %d %d Ghost picture %d %d\n", Mmo -> LayerMMO [0] . num_decoded_frame, Frame -> Picture -> poc, Frame -> x_size, Frame -> y_size);
		}else {
			*ImgToDisplay = 0;
		}

		//Remove picture from the frame buffer
		memmove(&Mmo -> poc_params[BestPos], &Mmo -> poc_params[BestPos + 1], (Mmo -> nb_img_display - Mmo -> num_free_poc_address - BestPos + 1) * sizeof(POC_PARAMS));
	}
}







/**
This function allows to display the right image according to its poc.
*/
void DisplayOrderSvc(SPS *sps, LIST_MMO *Current_pic, NAL *Nal, int *address_pic, int *x_size, 
					 int *y_size, int *Crop, int *ImgToDisplay, MMO *Mmo)
{


	//The picture management is conditioned if the current layer is allocated
	if (Mmo -> MemoryAllocation){
		//Just to avoid the AU parsing of players
		if(Nal -> DqIdMax != 255){
			ManageNewPic(Nal, Mmo, Current_pic, sps, Nal -> LayerId, Nal -> DqIdToDisplay);
		}else {
			//This case is only for the ParseAU used by TCPMP and mplayer.
			//The memory address has to be free
			//It stores into the Aumemory mangement to avoid problems
			//Be careful to SNR scalability which has got the same memory address than their base layer.
			int i;
			int store = 1;
			for (i = 0; i < Mmo -> AuMemory; i++){
				if(Mmo -> AuMemoryAddress[i] == Current_pic -> MemoryAddress){
					store = 0;
					break;
				}
			}
			if(store){
				Mmo -> AuMemoryAddress[Mmo -> AuMemory] = Current_pic -> MemoryAddress;
				Mmo -> AuMemory++; 
			}
		}

		//Detect if we have waited enough decoded frame except for the first frame.
		if (Nal -> DqIdMax == Nal -> DqId && ((Mmo -> nb_img_display - Mmo -> num_free_poc_address >= Mmo -> nb_img_display) 
			|| (!Mmo -> LayerMMO [0] . num_decoded_frame))){
				SearchFrameToDisplay(Nal, Mmo, x_size, y_size, Crop, ImgToDisplay, address_pic);
		}else{
			//Not ready yet
			*ImgToDisplay = 0;
			//To initialize for READ_DYNA_YUV
			*x_size = (sps -> pic_width_in_mbs) << 4;
			*y_size = (sps -> pic_height_in_map_units) << 4;
		}
	}
}


/**
This function empties the video buffer when the video is finished.
*/
void FlushSVCFrame(SPS *sps, NAL *Nal, MMO *Mmo, int *x_size, int *y_size, int *Crop, int *ImgToDisplay, int *address_pic)
{
	if(Mmo -> nb_img_display - Mmo -> num_free_poc_address == 0){
		exit(10000);
	}
	SearchFrameToDisplay(Nal, Mmo, x_size, y_size, Crop, ImgToDisplay, address_pic);
}
