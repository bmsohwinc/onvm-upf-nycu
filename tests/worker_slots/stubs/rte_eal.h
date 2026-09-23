#pragma once
enum rte_proc_type_t { RTE_PROC_PRIMARY, RTE_PROC_SECONDARY };
enum rte_proc_type_t rte_eal_process_type(void);
