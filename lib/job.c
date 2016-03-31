#include "imageflow_private.h"
#include "nodes.h"
#include "codecs.h"

struct flow_job* flow_job_create(flow_c* c)
{

    struct flow_job* job = (struct flow_job*)FLOW_malloc(c, sizeof(struct flow_job));
    if (job == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return NULL;
    }
    static int32_t job_id = 0;
    flow_job_configure_recording(c, job, false, false, false, false, false);
    job->next_graph_version = 0;
    job->debug_job_id = job_id++;
    job->codecs_head = NULL;
    job->codecs_tail = NULL;
    job->max_calc_flatten_execute_passes = 6;
    return job;
}

bool flow_job_configure_recording(flow_c* c, struct flow_job* job, bool record_graph_versions, bool record_frame_images,
                                  bool render_last_graph, bool render_graph_versions, bool render_animated_graph)
{
    job->record_frame_images = record_frame_images;
    job->record_graph_versions = record_graph_versions;
    job->render_last_graph = render_last_graph;
    job->render_graph_versions = render_graph_versions && record_graph_versions;
    job->render_animated_graph = render_animated_graph && job->render_graph_versions;
    return true;
}
bool flow_job_destroy(flow_c* c, struct flow_job* job) { return FLOW_destroy(c, job); }

struct flow_io* flow_job_get_io(flow_c* c, struct flow_job* job, int32_t placeholder_id)
{
    struct flow_codec_instance* codec = flow_job_get_codec_instance(c, job, placeholder_id);
    if (codec == NULL) {
        FLOW_add_to_callstack(c);
        return NULL;
    }
    return codec->io;
}

bool flow_job_get_output_buffer(flow_c* c, struct flow_job* job, int32_t placeholder_id,
                                uint8_t** out_pointer_to_buffer, size_t* out_length)
{
    struct flow_io* io = flow_job_get_io(c, job, placeholder_id);
    if (io == NULL) {
        FLOW_add_to_callstack(c);
        return false;
    }
    if (!flow_io_get_output_buffer(c, io, out_pointer_to_buffer, out_length)) {
        FLOW_add_to_callstack(c);
        return false;
    }
    return true;
}

bool flow_job_add_io(flow_c* c, struct flow_job* job, struct flow_io* io, int32_t placeholder_id,
                     FLOW_DIRECTION direction)
{
    struct flow_codec_instance* r
        = (struct flow_codec_instance*)FLOW_malloc_owned(c, sizeof(struct flow_codec_instance), job);
    if (r == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return false;
    }
    r->next = NULL;
    r->graph_placeholder_id = placeholder_id;
    r->io = io;
    r->codec_id = 0;
    r->codec_state = NULL;
    r->direction = direction;
    if (job->codecs_head == NULL) {
        job->codecs_head = r;
        job->codecs_tail = r;
    } else {
        job->codecs_tail->next = r;
        job->codecs_tail = r;
    }
    if (direction == FLOW_OUTPUT) {
        return true; // We don't determine output codecs this early.
    }

    uint8_t buffer[8];
    int64_t bytes_read = io->read_func(c, io, &buffer[0], 8);
    if (bytes_read != 8) {
        FLOW_error_msg(c, flow_status_IO_error, "Failed to read first 8 bytes of file");
        return false;
    }
    if (!io->seek_function(c, io, 0)) {
        FLOW_error_msg(c, flow_status_IO_error, "Failed to seek to byte 0 in file");
        return false;
    }

    int64_t ctype = flow_job_codec_select(c, job, &buffer[0], bytes_read);
    if (ctype == flow_codec_type_null) {
        // unknown
        FLOW_error_msg(c, flow_status_Not_implemented,
                       "Unrecognized leading byte sequence %02x%02x%02x%02x%02x%02x%02x%02x", buffer[0], buffer[1],
                       buffer[2], buffer[3], buffer[4], buffer[5], buffer[6],
                       buffer[7]); // Or bad buffer, unsupported file type, etc.
        return false;
    }
    r->codec_id = ctype;
    if (!flow_job_initialize_codec(c, job, r)) {
        FLOW_add_to_callstack(c);
        return false;
    }
    return true;
}

bool flow_job_execute(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{
    if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
        FLOW_error_return(c);
    }
    if (!flow_job_link_codecs(c, job, graph_ref)) {
        FLOW_error_return(c);
    }

    // States for a node
    // New
    // OutboundDimensionsKnown
    // Flattened
    // Optimized
    // LockedForExecution
    // Executed
    int32_t passes = 0;
    while (!flow_job_graph_fully_executed(c, job, *graph_ref)) {
        if (passes >= job->max_calc_flatten_execute_passes) {
            FLOW_error(c, flow_status_Maximum_graph_passes_exceeded);
            return false;
        }
        if (!flow_job_populate_dimensions_where_certain(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_graph_pre_optimize_flatten(c, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_populate_dimensions_where_certain(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_graph_optimize(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_populate_dimensions_where_certain(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_graph_post_optimize_flatten(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_populate_dimensions_where_certain(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
        if (!flow_job_execute_where_certain(c, job, graph_ref)) {
            FLOW_error_return(c);
        }
        passes++;

        if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
            FLOW_error_return(c);
        }
    }
    if (job->next_graph_version > 0 && job->render_last_graph
        && !flow_job_render_graph_to_png(c, job, *graph_ref, job->next_graph_version - 1)) {
        FLOW_error_return(c);
    }
    return true;
}

static bool node_visitor_post_optimize_flatten(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref,
                                               int32_t node_id, bool* quit, bool* skip_outbound_paths,
                                               void* custom_data)
{

    if (!flow_node_update_state(c, *graph_ref, node_id)) {
        FLOW_error_return(c);
    }
    struct flow_node* n = &(*graph_ref)->nodes[node_id];

    // If input nodes are populated
    if (n->state == flow_node_state_ReadyForPostOptimizeFlatten) {
        if (!flow_node_post_optimize_flatten(c, graph_ref, node_id)) {
            FLOW_error_return(c);
        }
        if (!flow_graph_validate(c, *graph_ref)) {
            FLOW_error_return(c);
        }
        *quit = true;
        *((bool*)custom_data) = true;
    } else if ((n->state & flow_node_state_InputDimensionsKnown) == 0) {
        // we can't flatten past missing dimensions
        *skip_outbound_paths = true;
    }
    return true;
}

bool flow_graph_post_optimize_flatten(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{
    if (*graph_ref == NULL) {
        FLOW_error(c, flow_status_Null_argument);
        return false;
    }
    bool re_walk;
    do {
        re_walk = false;
        if (!flow_graph_walk(c, job, graph_ref, node_visitor_post_optimize_flatten, NULL, &re_walk)) {
            FLOW_error_return(c);
        }
    } while (re_walk);
    return true;
}

static bool node_visitor_optimize(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref, int32_t node_id,
                                  bool* quit, bool* skip_outbound_paths, void* custom_data)
{

    struct flow_node* node = &(*graph_ref)->nodes[node_id];
    if (node->state == flow_node_state_ReadyForOptimize) {
        node->state = (flow_node_state)(node->state | flow_node_state_Optimized);
    }

    // Implement optimizations
    return true;
}

bool flow_graph_optimize(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{
    if (*graph_ref == NULL) {
        FLOW_error(c, flow_status_Null_argument);
        return false;
    }
    bool re_walk;
    do {
        re_walk = false;
        if (!flow_graph_walk(c, job, graph_ref, node_visitor_optimize, NULL, &re_walk)) {
            FLOW_error_return(c);
        }
    } while (re_walk);
    return true;
}

static bool flow_job_populate_dimensions_for_node(flow_c* c, struct flow_job* job, struct flow_graph* g,
                                                  int32_t node_id, bool force_estimate)
{
    uint64_t now = flow_get_high_precision_ticks();

    if (!flow_node_populate_dimensions(c, g, node_id, force_estimate)) {
        FLOW_error_return(c);
    }
    g->nodes[node_id].ticks_elapsed += flow_get_high_precision_ticks() - now;
    return true;
}

bool flow_node_has_dimensions(flow_c* c, struct flow_graph* g, int32_t node_id)
{
    struct flow_node* n = &g->nodes[node_id];
    return n->result_width > 0;
}

bool flow_node_inputs_have_dimensions(flow_c* c, struct flow_graph* g, int32_t node_id)
{
    int32_t i;
    for (i = 0; i < g->next_edge_id; i++) {
        if (g->edges[i].type != flow_edgetype_null && g->edges[i].to == node_id) {
            if (!flow_node_has_dimensions(c, g, g->edges[i].from)) {
                return false;
            }
        }
    }
    return true;
}

static bool node_visitor_dimensions(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref, int32_t node_id,
                                    bool* quit, bool* skip_outbound_paths, void* custom_data)
{

    struct flow_node* n = &(*graph_ref)->nodes[node_id];

    int32_t outbound_edges = flow_graph_get_edge_count(c, *graph_ref, node_id, false, flow_edgetype_null, false, true);
    if (outbound_edges == 0) {
        return true; // Endpoint node - no need.
    }
    if (!flow_node_has_dimensions(c, *graph_ref, node_id)) {
        if (!flow_node_update_state(c, *graph_ref, node_id)) {
            FLOW_error_return(c);
        }

        // If input nodes are populated
        if ((n->state & flow_node_state_InputDimensionsKnown) > 0) {
            if (!flow_job_populate_dimensions_for_node(c, job, *graph_ref, node_id, (bool)custom_data)) {
                FLOW_error_return(c);
            }
        }
        if (!flow_node_has_dimensions(c, *graph_ref, node_id)) {
            // We couldn't populate this edge, so we sure can't populate others in this direction.
            // Stop this branch of recursion
            *skip_outbound_paths = true;
        } else {
            flow_job_notify_graph_changed(c, job, *graph_ref);
        }
    }
    return true;
}

bool flow_job_populate_dimensions_where_certain(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{
    // TODO: would be good to verify graph is acyclic.
    if (!flow_graph_walk_dependency_wise(c, job, graph_ref, node_visitor_dimensions, NULL, (void*)false)) {
        FLOW_error_return(c);
    }
    return true;
}

bool flow_job_force_populate_dimensions(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{
    // TODO: would be good to verify graph is acyclic.
    if (!flow_graph_walk(c, job, graph_ref, node_visitor_dimensions, NULL, (void*)true)) {
        FLOW_error_return(c);
    }
    return true;
}

static bool flow_job_node_is_executed(flow_c* c, struct flow_job* job, struct flow_graph* g, int32_t node_id)
{
    return (g->nodes[node_id].state & flow_node_state_Executed) > 0;
}
bool flow_job_graph_fully_executed(flow_c* c, struct flow_job* job, struct flow_graph* g)
{
    int32_t i;
    for (i = 0; i < g->next_node_id; i++) {
        if (g->nodes[i].type != flow_ntype_Null) {
            if (!flow_job_node_is_executed(c, job, g, i)) {
                return false;
            }
        }
    }
    return true;
}

static bool node_visitor_execute(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref, int32_t node_id,
                                 bool* quit, bool* skip_outbound_paths, void* custom_data)
{

    if (!flow_node_update_state(c, *graph_ref, node_id)) {
        FLOW_error_return(c);
    }
    struct flow_node* n = &(*graph_ref)->nodes[node_id];

    if (!flow_job_node_is_executed(c, job, *graph_ref, node_id) && n->state == flow_node_state_ReadyForExecution) {
        uint64_t now = flow_get_high_precision_ticks();
        if (!flow_node_execute(c, job, *graph_ref, node_id)) {
            FLOW_error_return(c);
        } else {
            (*graph_ref)->nodes[node_id].ticks_elapsed += flow_get_high_precision_ticks() - now;
            n->state = (flow_node_state)(n->state | flow_node_state_Executed);
            flow_job_notify_node_complete(c, job, *graph_ref, node_id);
        }
    }
    if (!flow_job_node_is_executed(c, job, *graph_ref, node_id)) {
        // If we couldn't complete this node yet, end this branch.
        *skip_outbound_paths = true;
    } else {
        flow_job_notify_graph_changed(c, job, *graph_ref);
    }
    return true;
}

// if no hits, search forward

bool flow_job_execute_where_certain(flow_c* c, struct flow_job* job, struct flow_graph** g)
{
    if (*g == NULL) {
        FLOW_error(c, flow_status_Null_argument);
        return false;
    }

    //    //Resets and creates state tracking for this graph
    //    if (!flow_job_create_state(c,job, *g)){
    //        FLOW_error_return(c);
    //    }

    if (!flow_graph_walk_dependency_wise(c, job, g, node_visitor_execute, NULL, NULL)) {
        FLOW_error_return(c);
    }
    return true;
}

static bool node_visitor_flatten(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref, int32_t node_id,
                                 bool* quit, bool* skip_outbound_paths, void* custom_data)
{

    if (!flow_node_update_state(c, *graph_ref, node_id)) {
        FLOW_error_return(c);
    }
    struct flow_node* n = &(*graph_ref)->nodes[node_id];

    // If input nodes are populated
    if (n->state == flow_node_state_ReadyForPreOptimizeFlatten) {
        if (!flow_node_pre_optimize_flatten(c, graph_ref, node_id)) {
            FLOW_error_return(c);
        }
        *quit = true;
        *((bool*)custom_data) = true;
    } else if ((n->state & flow_node_state_InputDimensionsKnown) == 0) {
        // we can't flatten past missing dimensions
        *skip_outbound_paths = true;
    }
    return true;
}

bool flow_graph_pre_optimize_flatten(flow_c* c, struct flow_graph** graph_ref)
{
    if (*graph_ref == NULL) {
        FLOW_error(c, flow_status_Null_argument);
        return false;
    }
    bool re_walk;
    do {
        re_walk = false;
        if (!flow_graph_walk_dependency_wise(c, NULL, graph_ref, node_visitor_flatten, NULL, &re_walk)) {
            FLOW_error_return(c);
        }
    } while (re_walk);
    return true;
}

bool flow_job_link_codecs(flow_c* c, struct flow_job* job, struct flow_graph** graph_ref)
{

    if (graph_ref == NULL || *graph_ref == NULL) {
        FLOW_error(c, flow_status_Null_argument);
        return false;
    }
    if (!flow_job_notify_graph_changed(c, job, *graph_ref)) {
        FLOW_error_return(c);
    }

    struct flow_graph* g = *graph_ref;
    int32_t i;
    for (i = 0; i < g->next_node_id; i++) {
        if (g->nodes[i].type == flow_ntype_decoder || g->nodes[i].type == flow_ntype_encoder) {
            uint8_t* info_bytes = &g->info_bytes[g->nodes[i].info_byte_index];
            struct flow_nodeinfo_codec* info = (struct flow_nodeinfo_codec*)info_bytes;
            if (info->codec == NULL) {
                info->codec = flow_job_get_codec_instance(c, job, info->placeholder_id);

                if (info->codec == NULL)
                    FLOW_error_msg(c, flow_status_Graph_invalid,
                                   "No matching codec or io found for placeholder id %d (node #%d).",
                                   info->placeholder_id, i);
            }
        }
    }

    return true;
}

struct flow_codec_instance* flow_job_get_codec_instance(flow_c* c, struct flow_job* job, int32_t by_placeholder_id)
{
    struct flow_codec_instance* current = job->codecs_head;
    while (current != NULL) {
        if (current->graph_placeholder_id == by_placeholder_id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}
