# Crash Consistency (TODO)

If the graph driver is not shut down normally, the Docker database and layers in the graph driver need to be consistent. Each layer needs to be consistent as well. As the graph driver manages both Docker database and images/containers, these are kept in a consistent state by checkpointing. Thus this file system does not have the complexity of journaling schemes typically used in file systems to provide crash consistency.
