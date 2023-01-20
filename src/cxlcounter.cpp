//
// Created by victoryang00 on 1/12/23.
//

#include "cxlcounter.h"

void CXLSwitchEvent::inc_load() { load++; }
void CXLSwitchEvent::inc_store() { store++; }
void CXLSwitchEvent::inc_conflict() { conflict++; }
void CXLMemExpanderEvent::inc_load() { load++; }
void CXLMemExpanderEvent::inc_store() { store++; }
void CXLMemExpanderEvent::inc_migrate() { migrate++; }
void CXLMemExpanderEvent::inc_hit_old() { hit_old++; }
void CXLCounter::inc_local() { local++; }
void CXLCounter::inc_remote() { remote++; }
void CXLCounter::inc_hitm() { hitm++; }
