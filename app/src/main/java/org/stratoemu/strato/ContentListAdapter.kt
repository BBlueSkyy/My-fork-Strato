/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.res.ColorStateList
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import org.stratoemu.strato.databinding.ItemContentBinding

/**
 * Adapter for displaying game content items (updates and DLC)
 */
class ContentListAdapter(
    private val onUpdateToggled: (ContentItem, Boolean) -> Unit,
    private val onDlcToggled: (ContentItem, Boolean) -> Unit,
    private val onItemDelete: (ContentItem) -> Unit
) : ListAdapter<ContentItem, ContentListAdapter.ContentViewHolder>(ContentDiffCallback()) {
    
    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ContentViewHolder {
        val binding = ItemContentBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return ContentViewHolder(binding)
    }
    
    override fun onBindViewHolder(holder: ContentViewHolder, position: Int) {
        holder.bind(getItem(position))
    }
    
    inner class ContentViewHolder(
        private val binding: ItemContentBinding
    ) : RecyclerView.ViewHolder(binding.root) {
        
        fun bind(item: ContentItem) {
            Log.d("ContentListAdapter", "Binding item: $item")
            
            binding.contentName.text = if (item.type == ContentType.DLC) {
                val displayText = item.displayName ?: item.name
                Log.d("ContentListAdapter", "DLC display name: $displayText")
                displayText.ifEmpty { "DLC" }
            } else {
                val version = item.version?.removePrefix("v") ?: ""
                Log.d("ContentListAdapter", "Update version: $version")
                version
            }

            when (item.type) {
                ContentType.UPDATE -> {
                    binding.contentTypeIcon.setImageResource(R.drawable.ic_system_update)
                    binding.radioButton.visibility = View.VISIBLE
                    binding.radioButton.isChecked = item.isEnabled
                    binding.checkBox.visibility = View.GONE

                    binding.radioButton.jumpDrawablesToCurrentState()

                    val bgColor = MaterialColors.getColor(
                        binding.iconContainer,
                        com.google.android.material.R.attr.colorPrimaryContainer
                    )
                    val iconColor = MaterialColors.getColor(
                        binding.iconContainer,
                        com.google.android.material.R.attr.colorOnPrimaryContainer
                    )
                    binding.iconContainer.setCardBackgroundColor(bgColor)
                    binding.contentTypeIcon.imageTintList = ColorStateList.valueOf(iconColor)
                }
                ContentType.DLC -> {
                    binding.contentTypeIcon.setImageResource(R.drawable.ic_extension)
                    binding.checkBox.visibility = View.VISIBLE
                    binding.checkBox.isChecked = item.isEnabled
                    binding.radioButton.visibility = View.GONE

                    binding.checkBox.jumpDrawablesToCurrentState()

                    val bgColor = MaterialColors.getColor(
                        binding.iconContainer,
                        com.google.android.material.R.attr.colorSecondaryContainer
                    )
                    val iconColor = MaterialColors.getColor(
                        binding.iconContainer,
                        com.google.android.material.R.attr.colorOnSecondaryContainer
                    )
                    binding.iconContainer.setCardBackgroundColor(bgColor)
                    binding.contentTypeIcon.imageTintList = ColorStateList.valueOf(iconColor)
                }
            }

            binding.selectionContainer.visibility = View.VISIBLE
            
            // Handle radio button click (for updates)
            binding.radioButton.setOnClickListener {
                if (binding.radioButton.isChecked) {
                    onUpdateToggled.invoke(item, true)
                } else {
                    // Don't allow unchecking, as we need at least one update
                    binding.radioButton.isChecked = true
                }
            }
            
            // Handle checkbox click (for DLCs)
            binding.checkBox.setOnClickListener {
                onDlcToggled.invoke(item, binding.checkBox.isChecked)
            }
            
            // Handle delete button
            binding.deleteButton.setOnClickListener {
                onItemDelete(item)
            }
            
            // Handle item click (for DLCs, toggle the checkbox)
            binding.root.setOnClickListener {
                when (item.type) {
                    ContentType.UPDATE -> {
                        if (!binding.radioButton.isChecked) {
                            binding.radioButton.isChecked = true
                            onUpdateToggled(item, true)
                        }
                    }
                    ContentType.DLC -> {
                        binding.checkBox.isChecked = !binding.checkBox.isChecked
                        onDlcToggled(item, binding.checkBox.isChecked)
                    }
                }
            }
        }
    }
    
    private class ContentDiffCallback : DiffUtil.ItemCallback<ContentItem>() {
        override fun areItemsTheSame(oldItem: ContentItem, newItem: ContentItem): Boolean {
            return oldItem.uri == newItem.uri
        }
        
        override fun areContentsTheSame(oldItem: ContentItem, newItem: ContentItem): Boolean {
            return oldItem == newItem
        }
    }
}