//***************************************************************************
//* Copyright (c) 2019 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#pragma once

#include "common/pipeline/stage.hpp"

namespace debruijn_graph {

//todo rename
    class RestrictedEdgesFilling : public spades::AssemblyStage {
    public:
        RestrictedEdgesFilling()
                : AssemblyStage("Restricted Edges Filling", "restricted_edges_filling") {}

        void run(conj_graph_pack &gp, const char*);

        void FillRestrictedEdges(conj_graph_pack &gp);
    };

}
