library(ggplot2)
library(dplyr)

# 1. Load the data
if (!file.exists("cv_benchmark.csv")) {
  stop("CSV file not found! Run your benchmark first: ./MagMutex > cv_benchmark.csv")
}

df <- read.csv("cv_benchmark.csv")

# --- CONSOLE SUMMARY ---
# Let's print some quick results so you can see them in the terminal
cat("\n=== SUMMARY STATISTICS (Total Time in ms) ===\n")
summary_stats <- df %>%
  filter(role == "Producer") %>%
  group_by(mutex, threads) %>%
  summarise(
    mean_ms = mean(latency_ns) / 1e6,
    sd_ms = sd(latency_ns) / 1e6,
    .groups = 'drop'
  )
print(summary_stats)

# --- PLOT 1: THROUGHPUT ---
cat("\nGenerating Throughput Plot: throughput.png ...")
p1 <- ggplot(df %>% filter(role == "Producer"), aes(x = factor(threads), y = latency_ns / 1e6, fill = mutex)) +
  geom_boxplot() +
  labs(title = "Condition Variable Throughput (1M Items)",
       subtitle = "Lower is better (Total wall clock time)",
       x = "Thread Count (1 Prod + N Cons)",
       y = "Milliseconds") +
  theme_minimal() +
  scale_fill_brewer(palette = "Set1")

ggsave("throughput.png", p1, width = 8, height = 6)

# --- PLOT 2: FAIRNESS ---
cat("\nGenerating Fairness Plot: fairness.png ...")
# Look at the 16-thread case specifically
consumers_16 <- df %>% filter(threads == 16, role == "Consumer")
p2 <- ggplot(consumers_16, aes(x = factor(thread_id), y = items_processed, fill = mutex)) +
  geom_boxplot() +
  labs(title = "Work Distribution Fairness (16 Threads)",
       subtitle = "A 'fair' lock shows equal heights across all Thread IDs",
       x = "Consumer Thread ID",
       y = "Items Consumed") +
  theme_minimal() +
  scale_fill_brewer(palette = "Set1")

ggsave("fairness.png", p2, width = 10, height = 6)

cat("\n\nDone! Check your folder for 'throughput.png' and 'fairness.png'.\n")