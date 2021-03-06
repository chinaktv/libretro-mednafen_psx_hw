/***************************************************************************
*   Copyright (C) 2016 by iCatButler                                      *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
***************************************************************************/

/**************************************************************************
*	pgxp_gte.c
*	PGXP - Parallel/Precision Geometry Xform Pipeline
*
*	Created on: 12 Mar 2016
*      Author: iCatButler
***************************************************************************/

#include <string.h>
#include <math.h>

#include "pgxp_gte.h"
#include "pgxp_main.h"
#include "pgxp_value.h"
#include "pgxp_mem.h"
#include "pgxp_debug.h"
#include "pgxp_cpu.h"
#include "pgxp_gpu.h"


// GTE registers
PGXP_value GTE_data_reg_mem[32];
PGXP_value GTE_ctrl_reg_mem[32];


PGXP_value* GTE_data_reg = GTE_data_reg_mem;
PGXP_value* GTE_ctrl_reg = GTE_ctrl_reg_mem;

void PGXP_InitGTE()
{
	memset(GTE_data_reg_mem, 0, sizeof(GTE_data_reg_mem));
	memset(GTE_ctrl_reg_mem, 0, sizeof(GTE_ctrl_reg_mem));
}

// Instruction register decoding
#define op(_instr)		(_instr >> 26)			// The op part of the instruction register 
#define func(_instr)	((_instr) & 0x3F)		// The funct part of the instruction register 
#define sa(_instr)		((_instr >>  6) & 0x1F) // The sa part of the instruction register
#define rd(_instr)		((_instr >> 11) & 0x1F)	// The rd part of the instruction register 
#define rt(_instr)		((_instr >> 16) & 0x1F)	// The rt part of the instruction register 
#define rs(_instr)		((_instr >> 21) & 0x1F)	// The rs part of the instruction register 
#define imm(_instr)		(_instr & 0xFFFF)		// The immediate part of the instruction register

#define SX0 (GTE_data_reg[ 12 ].x)
#define SY0 (GTE_data_reg[ 12 ].y)
#define SX1 (GTE_data_reg[ 13 ].x)
#define SY1 (GTE_data_reg[ 13 ].y)
#define SX2 (GTE_data_reg[ 14 ].x)
#define SY2 (GTE_data_reg[ 14 ].y)

#define SXY0 (GTE_data_reg[ 12 ])
#define SXY1 (GTE_data_reg[ 13 ])
#define SXY2 (GTE_data_reg[ 14 ])
#define SXYP (GTE_data_reg[ 15 ])

void PGXP_pushSXYZ2f(float _x, float _y, float _z, unsigned int _v)
{
	static unsigned int uCount = 0;
	low_value temp;
	// push values down FIFO
	SXY0 = SXY1;
	SXY1 = SXY2;
	
	SXY2.x		= _x;
	SXY2.y		= _y;
	SXY2.z		= (PGXP_GetModes() & PGXP_TEXTURE_CORRECTION) ? _z : 1.f;
	SXY2.value	= _v;
	SXY2.flags	= VALID_ALL;
	SXY2.count	= uCount++;

	// cache value in GPU plugin
	temp.word = _v;
	if(PGXP_GetModes() & PGXP_VERTEX_CACHE)
		PGXP_CacheVertex(temp.x, temp.y, &SXY2);
	else
		PGXP_CacheVertex(0, 0, NULL);

#ifdef GTE_LOG
	GTE_LOG("PGXP_PUSH (%f, %f) %u %u|", SXY2.x, SXY2.y, SXY2.flags, SXY2.count);
#endif
}

void PGXP_pushSXYZ2s(s64 _x, s64 _y, s64 _z, u32 v)
{
	float fx = (float)(_x) / (float)(1 << 16);
	float fy = (float)(_y) / (float)(1 << 16);
	float fz = (float)(_z);

	//if(Config.PGXP_GTE)
		PGXP_pushSXYZ2f(fx, fy, fz, v);
}

#define VX(n) (psxRegs.CP2D.p[ n << 1 ].sw.l)
#define VY(n) (psxRegs.CP2D.p[ n << 1 ].sw.h)
#define VZ(n) (psxRegs.CP2D.p[ (n << 1) + 1 ].sw.l)

int PGXP_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2)
{
	Validate(&SXY0, sxy0);
	Validate(&SXY1, sxy1);
	Validate(&SXY2, sxy2);
	if (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_01) == VALID_01))// && Config.PGXP_GTE && (Config.PGXP_Mode > 0))
		return 1;
	return 0;
}

float PGXP_NCLIP()
{
	float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

	// ensure fractional values are not incorrectly rounded to 0
	float nclipAbs = fabs(nclip);
	if (( 0.1f < nclipAbs) && (nclipAbs < 1.f))
		nclip += (nclip < 0.f ? -1 : 1);

	//float AX = SX1 - SX0;
	//float AY = SY1 - SY0;

	//float BX = SX2 - SX0;
	//float BY = SY2 - SY0;

	//// normalise A and B
	//float mA = sqrt((AX*AX) + (AY*AY));
	//float mB = sqrt((BX*BX) + (BY*BY));

	//// calculate AxB to get Z component of C
	//float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

	return nclip;
}

static PGXP_value PGXP_MFC2_int(u32 reg)
{
	switch (reg) 
	{
	case 15:
		GTE_data_reg[reg] = SXYP = SXY2;
		break;
	}

	return GTE_data_reg[reg];
}


static void PGXP_MTC2_int(PGXP_value value, u32 reg)
{
	switch(reg)
	{
		case 15:
			// push FIFO
			SXY0 = SXY1;
			SXY1 = SXY2;
			SXY2 = value;
			SXYP = SXY2;
			break;

		case 31:
			return;
	}

	GTE_data_reg[reg] = value;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void MFC2(int reg) {
	psx_value val;
	val.d = GTE_data_reg[reg].value;
	switch (reg) {
	case 1:
	case 3:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
		GTE_data_reg[reg].value = (s32)val.sw.l;
		GTE_data_reg[reg].y = 0.f;
		break;

	case 7:
	case 16:
	case 17:
	case 18:
	case 19:
		GTE_data_reg[reg].value = (u32)val.w.l;
		GTE_data_reg[reg].y = 0.f;
		break;

	case 15:
		GTE_data_reg[reg] = SXY2;
		break;

	case 28:
	case 29:
	//	psxRegs.CP2D.p[reg].d = LIM(IR1 >> 7, 0x1f, 0, 0) | (LIM(IR2 >> 7, 0x1f, 0, 0) << 5) | (LIM(IR3 >> 7, 0x1f, 0, 0) << 10);
		break;
	}
}

void PGXP_GTE_MFC2(u32 instr, u32 rtVal, u32 rdVal)
{
	// CPU[Rt] = GTE_D[Rd]
	Validate(&GTE_data_reg[rd(instr)], rdVal);
	//MFC2(rd(instr));
	CPU_reg[rt(instr)] = GTE_data_reg[rd(instr)];
	CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_MTC2(u32 instr, u32 rdVal, u32 rtVal)
{
	// GTE_D[Rd] = CPU[Rt]
	Validate(&CPU_reg[rt(instr)], rtVal);
	PGXP_MTC2_int(CPU_reg[rt(instr)], rd(instr));
	GTE_data_reg[rd(instr)].value = rdVal;
}

void PGXP_GTE_CFC2(u32 instr, u32 rtVal, u32 rdVal)
{
	// CPU[Rt] = GTE_C[Rd]
	Validate(&GTE_ctrl_reg[rd(instr)], rdVal);
	CPU_reg[rt(instr)] = GTE_ctrl_reg[rd(instr)];
	CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_CTC2(u32 instr, u32 rdVal, u32 rtVal)
{
	// GTE_C[Rd] = CPU[Rt]
	Validate(&CPU_reg[rt(instr)], rtVal);
	GTE_ctrl_reg[rd(instr)] = CPU_reg[rt(instr)];
	GTE_ctrl_reg[rd(instr)].value = rdVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void	PGXP_GTE_LWC2(u32 instr, u32 rtVal, u32 addr)
{
	// GTE_D[Rt] = Mem[addr]
	PGXP_value val;
	ValidateAndCopyMem(&val, addr, rtVal);
	PGXP_MTC2_int(val, rt(instr));
}

void	PGXP_GTE_SWC2(u32 instr, u32 rtVal, u32 addr)
{
	//  Mem[addr] = GTE_D[Rt]
	Validate(&GTE_data_reg[rt(instr)], rtVal);
	WriteMem(&GTE_data_reg[rt(instr)], addr);
}
