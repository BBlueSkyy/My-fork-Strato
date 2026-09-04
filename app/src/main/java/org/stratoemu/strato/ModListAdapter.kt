/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import org.stratoemu.strato.databinding.ItemContentBinding

class ModListAdapter(
    private val onToggled: (GameMod, Boolean) -> Unit,
    private val onDelete: (GameMod) -> Unit
) : ListAdapter<GameMod, ModListAdapter.ModViewHolder>(DiffCallback()) {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ModViewHolder {
        return ModViewHolder(
            ItemContentBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        )
    }

    override fun onBindViewHolder(holder: ModViewHolder, position: Int) {
        holder.bind(getItem(position))
    }

    inner class ModViewHolder(
        private val binding: ItemContentBinding
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(mod: GameMod) {
            binding.contentName.text = mod.name
            binding.contentTypeIcon.setImageResource(R.drawable.ic_extension)
            binding.radioButton.visibility = View.GONE
            binding.checkBox.visibility = View.VISIBLE
            binding.checkBox.isChecked = mod.enabled
            binding.checkBox.jumpDrawablesToCurrentState()
            binding.selectionContainer.visibility = View.VISIBLE

            val background = MaterialColors.getColor(
                binding.iconContainer,
                com.google.android.material.R.attr.colorSecondaryContainer
            )
            val foreground = MaterialColors.getColor(
                binding.iconContainer,
                com.google.android.material.R.attr.colorOnSecondaryContainer
            )
            binding.iconContainer.setCardBackgroundColor(background)
            binding.contentTypeIcon.imageTintList = ColorStateList.valueOf(foreground)

            binding.checkBox.setOnClickListener {
                onToggled(mod, binding.checkBox.isChecked)
            }
            binding.root.setOnClickListener {
                binding.checkBox.isChecked = !binding.checkBox.isChecked
                onToggled(mod, binding.checkBox.isChecked)
            }
            binding.deleteButton.setOnClickListener { onDelete(mod) }
        }
    }

    private class DiffCallback : DiffUtil.ItemCallback<GameMod>() {
        override fun areItemsTheSame(oldItem: GameMod, newItem: GameMod): Boolean {
            return oldItem.directory == newItem.directory
        }

        override fun areContentsTheSame(oldItem: GameMod, newItem: GameMod): Boolean {
            return oldItem == newItem
        }
    }
}
