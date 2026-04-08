#!/usr/bin/env Rscript

# MagMutex is a mutual exclusion(mutex) lock which is handcrafted and optimized,
# written in C23 with a certain amount of GNUism. Runs in mostly User Space.
# This script creates a plot 
# comparing MagMutex, Apple's POSIX Mutex(pthread_mutex) and CPython's PyMutex

# --- Setup ---
if (!requireNamespace("pacman", quietly = TRUE)) install.packages("pacman")
pacman::p_load(ggplot2, dplyr, rstudioapi, Hmisc) # Hmisc is needed for mean_cl_boot

# Environment-aware pathing
get_path <- function() {
  if (rstudioapi::isAvailable()) return(dirname(rstudioapi::getActiveDocumentContext()$path))
  args <- commandArgs(trailingOnly = FALSE)
  file_arg <- grep("--file=", args, value = TRUE)
  if (length(file_arg) > 0) return(dirname(normalizePath(sub("--file=", "", file_arg))))
  return(getwd())
} 

setwd(get_path())

# --- 1. Load Data (Using fast read if available) ---
# If you have data.table installed, use fread() instead for speed
df <- read.csv("benchmark_results.csv")

# --- 2. Process ---
trial_summary <- df %>%
  group_by(mutex, threads, trial) %>%
  summarise(max_lat = max(latency_ns, na.rm = TRUE), .groups = "drop")

# --- 3. Plot: Latency Scaling ---
p1 <- ggplot(trial_summary, aes(x = factor(threads), y = max_lat, color = mutex, group = mutex)) +
  stat_summary(fun = mean, geom = "line", linewidth = 1) +
  stat_summary(fun.data = mean_cl_boot, geom = "errorbar", width = 0.2) +
  # Using log scale to see performance differences across orders of magnitude
  scale_y_log10() + 
  labs(title = "Mutex Contention Scaling (Tail Latency)",
       subtitle = "Log scale. Error bars: 95% CI of trial maximums.",
       x = "Thread Count",
       y = "Nanoseconds (Log10)") +
  theme_minimal() +
  scale_color_brewer(palette = "Set1")

# --- 4. Plot: Distribution at Max Threads ---
max_threads <- max(df$threads)
p2 <- ggplot(df %>% filter(threads == max_threads), aes(x = mutex, y = latency_ns, fill = mutex)) +
  geom_violin(alpha = 0.5) +
  geom_boxplot(width = 0.1, outlier.size = 0.5, outlier.alpha = 0.5) +
  scale_y_log10() +
  labs(title = paste("Latency Distribution at", max_threads, "Threads"),
       y = "ns / op (Log Scale)",
       x = "Mutex Type") +
  theme_minimal() +
  guides(fill = "none")

# --- Save Outputs ---
ggsave("scaling_plot.png", p1, width = 10, height = 6)
ggsave("distribution_plot.png", p2, width = 10, height = 6)

# Explicitly render the plots in the RStudio Plots pane
print(p1)
print(p2)

cat("Done. Plots saved to:", get_path(), "\n")
